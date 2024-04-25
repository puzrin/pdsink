/* Copyright 2015 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "adc.h"
#include "common.h"
#include "config.h"
#include "crc.h"
#include "gpio.h"
#include "host_command.h"
#include "registers.h"
#include "timer.h"
#include "usb_pd.h"
#include "usb_pd_config.h"
#include "util.h"

#define CPRINTF(format, args...)
static const int debug_level;

/* Encode 5 bits using Biphase Mark Coding */
#define BMC(x)                                               \
	((x & 1 ? 0x001 : 0x3FF) ^ (x & 2 ? 0x004 : 0x3FC) ^ \
	 (x & 4 ? 0x010 : 0x3F0) ^ (x & 8 ? 0x040 : 0x3C0) ^ \
	 (x & 16 ? 0x100 : 0x300))

/* 4b/5b + Bimark Phase encoding */
static const uint16_t bmc4b5b[] = {
	/* 0 = 0000 */ BMC(0x1E) /* 11110 */,
	/* 1 = 0001 */ BMC(0x09) /* 01001 */,
	/* 2 = 0010 */ BMC(0x14) /* 10100 */,
	/* 3 = 0011 */ BMC(0x15) /* 10101 */,
	/* 4 = 0100 */ BMC(0x0A) /* 01010 */,
	/* 5 = 0101 */ BMC(0x0B) /* 01011 */,
	/* 6 = 0110 */ BMC(0x0E) /* 01110 */,
	/* 7 = 0111 */ BMC(0x0F) /* 01111 */,
	/* 8 = 1000 */ BMC(0x12) /* 10010 */,
	/* 9 = 1001 */ BMC(0x13) /* 10011 */,
	/* A = 1010 */ BMC(0x16) /* 10110 */,
	/* B = 1011 */ BMC(0x17) /* 10111 */,
	/* C = 1100 */ BMC(0x1A) /* 11010 */,
	/* D = 1101 */ BMC(0x1B) /* 11011 */,
	/* E = 1110 */ BMC(0x1C) /* 11100 */,
	/* F = 1111 */ BMC(0x1D) /* 11101 */,
	/* Sync-1      K-code       11000 Startsynch #1 */
	/* Sync-2      K-code       10001 Startsynch #2 */
	/* RST-1       K-code       00111 Hard Reset #1 */
	/* RST-2       K-code       11001 Hard Reset #2 */
	/* EOP         K-code       01101 EOP End Of Packet */
	/* Reserved    Error        00000 */
	/* Reserved    Error        00001 */
	/* Reserved    Error        00010 */
	/* Reserved    Error        00011 */
	/* Reserved    Error        00100 */
	/* Reserved    Error        00101 */
	/* Reserved    Error        00110 */
	/* Reserved    Error        01000 */
	/* Reserved    Error        01100 */
	/* Reserved    Error        10000 */
	/* Reserved    Error        11111 */
};

static const uint8_t dec4b5b[] = {
	/* Error    */ 0x10 /* 00000 */,
	/* Error    */ 0x10 /* 00001 */,
	/* Error    */ 0x10 /* 00010 */,
	/* Error    */ 0x10 /* 00011 */,
	/* Error    */ 0x10 /* 00100 */,
	/* Error    */ 0x10 /* 00101 */,
	/* Error    */ 0x10 /* 00110 */,
	/* RST-1    */ 0x13 /* 00111 K-code: Hard Reset #1 */,
	/* Error    */ 0x10 /* 01000 */,
	/* 1 = 0001 */ 0x01 /* 01001 */,
	/* 4 = 0100 */ 0x04 /* 01010 */,
	/* 5 = 0101 */ 0x05 /* 01011 */,
	/* Error    */ 0x10 /* 01100 */,
	/* EOP      */ 0x15 /* 01101 K-code: EOP End Of Packet */,
	/* 6 = 0110 */ 0x06 /* 01110 */,
	/* 7 = 0111 */ 0x07 /* 01111 */,
	/* Error    */ 0x10 /* 10000 */,
	/* Sync-2   */ 0x12 /* 10001 K-code: Startsynch #2 */,
	/* 8 = 1000 */ 0x08 /* 10010 */,
	/* 9 = 1001 */ 0x09 /* 10011 */,
	/* 2 = 0010 */ 0x02 /* 10100 */,
	/* 3 = 0011 */ 0x03 /* 10101 */,
	/* A = 1010 */ 0x0A /* 10110 */,
	/* B = 1011 */ 0x0B /* 10111 */,
	/* Sync-1   */ 0x11 /* 11000 K-code: Startsynch #1 */,
	/* RST-2    */ 0x14 /* 11001 K-code: Hard Reset #2 */,
	/* C = 1100 */ 0x0C /* 11010 */,
	/* D = 1101 */ 0x0D /* 11011 */,
	/* E = 1110 */ 0x0E /* 11100 */,
	/* F = 1111 */ 0x0F /* 11101 */,
	/* 0 = 0000 */ 0x00 /* 11110 */,
	/* Error    */ 0x10 /* 11111 */,
};

/* Start of Packet sequence : three Sync-1 K-codes, then one Sync-2 K-code */
#define PD_SOP \
	(PD_SYNC1 | (PD_SYNC1 << 5) | (PD_SYNC1 << 10) | (PD_SYNC2 << 15))
#define PD_SOP_PRIME \
	(PD_SYNC1 | (PD_SYNC1 << 5) | (PD_SYNC3 << 10) | (PD_SYNC3 << 15))
#define PD_SOP_PRIME_PRIME \
	(PD_SYNC1 | (PD_SYNC3 << 5) | (PD_SYNC1 << 10) | (PD_SYNC3 << 15))

/* Hard Reset sequence : three RST-1 K-codes, then one RST-2 K-code */
#define PD_HARD_RESET \
	(PD_RST1 | (PD_RST1 << 5) | (PD_RST1 << 10) | (PD_RST2 << 15))

/*
 * Polarity based on 'DFP Perspective' (see table USB Type-C Cable and Connector
 * Specification)
 *
 * CC1    CC2    STATE             POSITION
 * ----------------------------------------
 * open   open   NC                N/A
 * Rd     open   UFP attached      1
 * open   Rd     UFP attached      2
 * open   Ra     pwr cable no UFP  N/A
 * Ra     open   pwr cable no UFP  N/A
 * Rd     Ra     pwr cable & UFP   1
 * Ra     Rd     pwr cable & UFP   2
 * Rd     Rd     dbg accessory     N/A
 * Ra     Ra     audio accessory   N/A
 *
 * Note, V(Rd) > V(Ra)
 */
#ifndef PD_SRC_RD_THRESHOLD
#define PD_SRC_RD_THRESHOLD PD_SRC_DEF_RD_THRESH_MV
#endif
#ifndef PD_SRC_VNC
#define PD_SRC_VNC PD_SRC_DEF_VNC_MV
#endif

#ifndef CC_RA
#define CC_RA(port, cc, sel) (cc < PD_SRC_RD_THRESHOLD)
#endif
#define CC_RD(cc) ((cc >= PD_SRC_RD_THRESHOLD) && (cc < PD_SRC_VNC))
#ifndef CC_NC
#define CC_NC(port, cc, sel) (cc >= PD_SRC_VNC)
#endif

/*
 * Polarity based on 'UFP Perspective'.
 *
 * CC1    CC2    STATE             POSITION
 * ----------------------------------------
 * open   open   NC                N/A
 * Rp     open   DFP attached      1
 * open   Rp     DFP attached      2
 * Rp     Rp     Accessory attached N/A
 */
#ifndef PD_SNK_VA
#define PD_SNK_VA PD_SNK_VA_MV
#endif

#define CC_RP(cc) (cc >= PD_SNK_VA)

/*
 * Type C power source charge current limits are identified by their cc
 * voltage (set by selecting the proper Rd resistor). Any voltage below
 * TYPE_C_SRC_500_THRESHOLD will not be identified as a type C charger.
 */
#define TYPE_C_SRC_500_THRESHOLD PD_SRC_RD_THRESHOLD
#define TYPE_C_SRC_1500_THRESHOLD 660 /* mV */
#define TYPE_C_SRC_3000_THRESHOLD 1230 /* mV */

/* Convert TCPC Alert register to index into pd.alert[] */
#define ALERT_REG_TO_INDEX(reg) (reg - TCPC_REG_ALERT)

/* PD transmit errors */
enum pd_tx_errors {
	PD_TX_ERR_GOODCRC = -1, /* Failed to receive goodCRC */
	PD_TX_ERR_DISABLED = -2, /* Attempted transmit even though disabled */
	PD_TX_ERR_INV_ACK = -4, /* Received different packet instead of gCRC */
	PD_TX_ERR_COLLISION = -5 /* Collision detected during transmit */
};

/* PD Header with SOP* encoded in bits 31 - 28 */
union pd_header_sop {
	uint16_t pd_header;
	uint32_t head;
};

/*
 * If TCPM is not on this chip, and PD low power is defined, then use low
 * power task delay logic.
 */

/*
 * Receive message buffer size. Buffer physical size is RX_BUFFER_SIZE + 1,
 * but only RX_BUFFER_SIZE of that memory is used to store messages that can
 * be retrieved from TCPM. The last slot is a temporary buffer for collecting
 * a message before deciding whether or not to keep it.
 */
#define RX_BUFFER_SIZE 1

static struct pd_port_controller {
	/* current port power role (SOURCE or SINK) */
	uint8_t power_role;
	/* current port data role (DFP or UFP) */
	uint8_t data_role;
	/* Port polarity : 0 => CC1 is CC line, 1 => CC2 is CC line */
	uint8_t polarity;
	/* Our CC pull resistor setting */
	uint8_t cc_pull;
	/* CC status */
	uint8_t cc_status[2];
	/* TCPC alert status */
	uint16_t alert;
	uint16_t alert_mask;
	/* RX enabled */
	uint8_t rx_enabled;
	/* Power status */
	uint8_t power_status;
	uint8_t power_status_mask;


	/* Last received */
	int rx_head[RX_BUFFER_SIZE + 1];
	uint32_t rx_payload[RX_BUFFER_SIZE + 1][7];
	int rx_buf_head, rx_buf_tail;

	/* Next transmit */
	enum tcpci_msg_type tx_type;
	uint16_t tx_head;
	uint32_t tx_payload[7];
	const uint32_t *tx_data;
} pd[CONFIG_USB_PD_PORT_MAX_COUNT];

static int rx_buf_is_full(int port)
{
	/*
	 * TODO: Refactor these to use the incrementing-counter idiom instead of
	 * the wrapping-counter idiom to reclaim the last buffer entry.
	 *
	 * Buffer is full if the tail is 1 ahead of head.
	 */
	int diff = pd[port].rx_buf_tail - pd[port].rx_buf_head;
	return (diff == 1) || (diff == -RX_BUFFER_SIZE);
}

int rx_buf_is_empty(int port)
{
	/* Buffer is empty if the head and tail are the same */
	return pd[port].rx_buf_tail == pd[port].rx_buf_head;
}

void rx_buf_clear(int port)
{
	pd[port].rx_buf_tail = pd[port].rx_buf_head;
}

static void rx_buf_increment(int port, int *buf_ptr)
{
	*buf_ptr = *buf_ptr == RX_BUFFER_SIZE ? 0 : *buf_ptr + 1;
}

static inline int encode_short(int port, int off, uint16_t val16)
{
	off = pd_write_sym(port, off, bmc4b5b[(val16 >> 0) & 0xF]);
	off = pd_write_sym(port, off, bmc4b5b[(val16 >> 4) & 0xF]);
	off = pd_write_sym(port, off, bmc4b5b[(val16 >> 8) & 0xF]);
	return pd_write_sym(port, off, bmc4b5b[(val16 >> 12) & 0xF]);
}

int encode_word(int port, int off, uint32_t val32)
{
	off = encode_short(port, off, (val32 >> 0) & 0xFFFF);
	return encode_short(port, off, (val32 >> 16) & 0xFFFF);
}

/* prepare a 4b/5b-encoded PD message to send */
int prepare_message(int port, uint16_t header, uint8_t cnt,
		    const uint32_t *data)
{
	int off, i;
	/* 64-bit preamble */
	off = pd_write_preamble(port);
	/* Start Of Packet: 3x Sync-1 + 1x Sync-2 */
	off = pd_write_sym(port, off, BMC(PD_SYNC1));
	off = pd_write_sym(port, off, BMC(PD_SYNC1));
	off = pd_write_sym(port, off, BMC(PD_SYNC1));
	off = pd_write_sym(port, off, BMC(PD_SYNC2));
	/* header */
	off = encode_short(port, off, header);


	crc32_init();
	crc32_hash16(header);
	/* data payload */
	for (i = 0; i < cnt; i++) {
		off = encode_word(port, off, data[i]);
		crc32_hash32(data[i]);
	}
	/* CRC */
	off = encode_word(port, off, crc32_result());


	/* End Of Packet */
	off = pd_write_sym(port, off, BMC(PD_EOP));
	/* Ensure that we have a final edge */
	return pd_write_last_edge(port, off);
}

static int send_hard_reset(int port)
{
	int off;

	if (debug_level >= 1)
		CPRINTF("C%d Send hard reset\n", port);

	/* 64-bit preamble */
	off = pd_write_preamble(port);
	/* Hard-Reset: 3x RST-1 + 1x RST-2 */
	off = pd_write_sym(port, off, BMC(PD_RST1));
	off = pd_write_sym(port, off, BMC(PD_RST1));
	off = pd_write_sym(port, off, BMC(PD_RST1));
	off = pd_write_sym(port, off, BMC(PD_RST2));
	/* Ensure that we have a final edge */
	off = pd_write_last_edge(port, off);
	/* Transmit the packet */
	if (pd_start_tx(port, pd[port].polarity, off) < 0)
		return PD_TX_ERR_COLLISION;
	pd_tx_done(port, pd[port].polarity);
	/* Keep RX monitoring on */
	pd_rx_enable_monitoring(port);
	return 0;
}

static int send_validate_message(int port, uint16_t header,
				 const uint32_t *data)
{
	int r;
	static uint32_t payload[7];
	uint8_t expected_msg_id = PD_HEADER_ID(header);
	uint8_t cnt = PD_HEADER_CNT(header);
	int retries = PD_HEADER_TYPE(header) == PD_DATA_SOURCE_CAP ?
			      0 :
			      CONFIG_PD_RETRY_COUNT;

	/* retry 3 times if we are not getting a valid answer */
	for (r = 0; r <= retries; r++) {
		int bit_len, head;
		/* write the encoded packet in the transmission buffer */
		bit_len = prepare_message(port, header, cnt, data);
		/* Transmit the packet */
		if (pd_start_tx(port, pd[port].polarity, bit_len) < 0) {
			/*
			 * Collision detected, return immediately so we can
			 * respond to what we have received.
			 */
			return PD_TX_ERR_COLLISION;
		}
		pd_tx_done(port, pd[port].polarity);
		/*
		 * If this is the first attempt, leave RX monitoring off,
		 * and do a blocking read of the channel until timeout or
		 * packet received. If we failed the first try, enable
		 * interrupt and yield to other tasks, so that we don't
		 * starve them.
		 */
		if (r) {
			pd_rx_enable_monitoring(port);
			/* Wait for message receive timeout */
			if (task_wait_event(USB_PD_RX_TMOUT_US) ==
			    TASK_EVENT_TIMER)
				continue;
			/*
			 * Make sure we woke up due to rx recd, otherwise
			 * we need to manually start
			 */
			if (!pd_rx_started(port)) {
				pd_rx_disable_monitoring(port);
				pd_rx_start(port);
			}
		} else {
			/* starting waiting for GoodCrc */
			pd_rx_start(port);
		}
		/* read the incoming packet if any */
		head = pd_analyze_rx(port, payload);
		pd_rx_complete(port);
		/* keep RX monitoring on to avoid collisions */
		pd_rx_enable_monitoring(port);
		if (head > 0) { /* we got a good packet, analyze it */
			int type = PD_HEADER_TYPE(head);
			int nb = PD_HEADER_CNT(head);
			uint8_t id = PD_HEADER_ID(head);
			if (type == PD_CTRL_GOOD_CRC && nb == 0 &&
			    id == expected_msg_id) {
				/* got the GoodCRC we were expecting */
				/* do not catch last edges as a new packet */
				udelay(20);
				return bit_len;
			} else {
				/*
				 * we have received a good packet
				 * but not the expected GoodCRC,
				 * the other side is trying to contact us,
				 * bail out immediately so we can get the retry.
				 */
				return PD_TX_ERR_INV_ACK;
			}
		}
	}
	/* we failed all the re-transmissions */
	if (debug_level >= 1)
		CPRINTF("TX NOACK%d %04x/%d\n", port, header, cnt);
	return PD_TX_ERR_GOODCRC;
}

static void send_goodcrc(int port, int id)
{
	uint16_t header = PD_HEADER(PD_CTRL_GOOD_CRC, pd[port].power_role,
				    pd[port].data_role, id, 0, 0, 0);
	int bit_len = prepare_message(port, header, 0, NULL);

	if (pd_start_tx(port, pd[port].polarity, bit_len) < 0)
		/* another packet recvd before we could send goodCRC */
		return;
	pd_tx_done(port, pd[port].polarity);
	/* Keep RX monitoring on */
	pd_rx_enable_monitoring(port);
}

#if 0
/* TODO: when/how do we trigger this ? */
static int analyze_rx_bist(int port);

void bist_mode_2_rx(int port)
{
	int analyze_bist = 0;
	int num_bits;
	timestamp_t start_time;

	/* monitor for incoming packet */
	pd_rx_enable_monitoring(port);

	/* loop until we start receiving data */
	start_time.val = get_time().val;
	while ((get_time().val - start_time.val) < (500*MSEC)) {
		task_wait_event(10*MSEC);
		/* incoming packet ? */
		if (pd_rx_started(port)) {
			analyze_bist = 1;
			break;
		}
	}

	if (analyze_bist) {
		/*
		 * once we start receiving bist data, analyze 40 bytes
		 * every 10 msec. Continue analyzing until BIST data
		 * is no longer received. The standard limits the max
		 * BIST length to 60 msec.
		 */
		start_time.val = get_time().val;
		while ((get_time().val - start_time.val)
			< (PD_T_BIST_RECEIVE)) {
			num_bits = analyze_rx_bist(port);
			pd_rx_complete(port);
			/*
			 * If no data was received, then analyze_rx_bist()
			 * will return a -1 and there is no need to stay
			 * in this mode
			 */
			if (num_bits == -1)
				break;
			msleep(10);
			pd_rx_enable_monitoring(port);
		}
	} else {
		CPRINTF("BIST RX TO\n");
	}
}
#endif

static void bist_mode_2_tx(int port)
{
	int bit;

	CPRINTF("BIST 2: p%d\n", port);
	/*
	 * build context buffer with 5 bytes, where the data is
	 * alternating 1's and 0's.
	 */
	bit = pd_write_sym(port, 0, BMC(0x15));
	bit = pd_write_sym(port, bit, BMC(0x0a));
	bit = pd_write_sym(port, bit, BMC(0x15));
	bit = pd_write_sym(port, bit, BMC(0x0a));

	/* start a circular DMA transfer */
	pd_tx_set_circular_mode(port);
	pd_start_tx(port, pd[port].polarity, bit);

	task_wait_event(PD_T_BIST_TRANSMIT);

	/* clear dma circular mode, will also stop dma */
	pd_tx_clear_circular_mode(port);
	/* finish and cleanup transmit */
	pd_tx_done(port, pd[port].polarity);
}

static inline int decode_short(int port, int off, uint16_t *val16)
{
	uint32_t w;
	int end;

	end = pd_dequeue_bits(port, off, 20, &w);

#if 0 /* DEBUG */
	CPRINTS("%d-%d: %05x %x:%x:%x:%x",
		off, end, w,
		dec4b5b[(w >> 15) & 0x1f], dec4b5b[(w >> 10) & 0x1f],
		dec4b5b[(w >>  5) & 0x1f], dec4b5b[(w >>  0) & 0x1f]);
#endif
	*val16 = dec4b5b[w & 0x1f] | (dec4b5b[(w >> 5) & 0x1f] << 4) |
		 (dec4b5b[(w >> 10) & 0x1f] << 8) |
		 (dec4b5b[(w >> 15) & 0x1f] << 12);
	return end;
}

static inline int decode_word(int port, int off, uint32_t *val32)
{
	off = decode_short(port, off, (uint16_t *)val32);
	return decode_short(port, off, ((uint16_t *)val32 + 1));
}


int pd_analyze_rx(int port, uint32_t *payload)
{
	int bit;
	char *msg = "---";
	uint32_t val = 0;
	union pd_header_sop phs;
	uint32_t pcrc, ccrc;
	int p, cnt;
	uint32_t eop;

	pd_init_dequeue(port);

	/* Detect preamble */
	bit = pd_find_preamble(port);
	if (bit == PD_RX_ERR_HARD_RESET || bit == PD_RX_ERR_CABLE_RESET) {
		/* Hard reset or cable reset */
		return bit;
	} else if (bit < 0) {
		msg = "Preamble";
		goto packet_err;
	}

	/* Find the Start Of Packet sequence */
	while (bit > 0) {
		bit = pd_dequeue_bits(port, bit, 20, &val);
		if (val == PD_SOP) {
			break;
		} else if (val == PD_SOP_PRIME) {
			CPRINTF("SOP'\n");
			return PD_RX_ERR_UNSUPPORTED_SOP;
		} else if (val == PD_SOP_PRIME_PRIME) {
			CPRINTF("SOP''\n");
			return PD_RX_ERR_UNSUPPORTED_SOP;
		}
	}
	if (bit < 0) {
		msg = "SOP";
		goto packet_err;
	}

	phs.head = 0;

	/* read header */
	bit = decode_short(port, bit, &phs.pd_header);


	crc32_init();
	crc32_hash16(phs.pd_header);
	cnt = PD_HEADER_CNT(phs.pd_header);


	/* read payload data */
	for (p = 0; p < cnt && bit > 0; p++) {
		bit = decode_word(port, bit, payload + p);
		crc32_hash32(payload[p]);
	}
	ccrc = crc32_result();


	if (bit < 0) {
		msg = "len";
		goto packet_err;
	}

	/* check transmitted CRC */
	bit = decode_word(port, bit, &pcrc);
	if (bit < 0 || pcrc != ccrc) {
		msg = "CRC";
		if (pcrc != ccrc)
			bit = PD_RX_ERR_CRC;
		if (debug_level >= 1)
			CPRINTF("CRC%d %08x <> %08x\n", port, pcrc, ccrc);
		goto packet_err;
	}

	/*
	 * Check EOP. EOP is 5 bits, but last bit may not be able to
	 * be dequeued, depending on ending state of CC line, so stop
	 * at 4 bits (assumes last bit is 0).
	 */
	bit = pd_dequeue_bits(port, bit, 4, &eop);
	if (bit < 0 || eop != PD_EOP) {
		msg = "EOP";
		goto packet_err;
	}

	return phs.head;
packet_err:
	if (debug_level >= 2)
		pd_dump_packet(port, msg);
	else
		CPRINTF("RXERR%d %s\n", port, msg);
	return bit;
}

static void handle_request(int port, uint16_t head)
{
	int cnt = PD_HEADER_CNT(head);

	if (PD_HEADER_TYPE(head) != PD_CTRL_GOOD_CRC || cnt)
		send_goodcrc(port, PD_HEADER_ID(head));
	else
		/* keep RX monitoring on to avoid collisions */
		pd_rx_enable_monitoring(port);
}

/* Convert CC voltage to CC status */
static int cc_voltage_to_status(int port, int cc_volt, int cc_sel)
{
	/* If we have a pull-up, then we are source, check for Rd. */
	if (pd[port].cc_pull == TYPEC_CC_RP) {
		if (CC_NC(port, cc_volt, cc_sel))
			return TYPEC_CC_VOLT_OPEN;
		else if (CC_RA(port, cc_volt, cc_sel))
			return TYPEC_CC_VOLT_RA;
		else
			return TYPEC_CC_VOLT_RD;
		/* If we have a pull-down, then we are sink, check for Rp. */
	}
	/* If we are open, then always return 0 */
	else
		return 0;
}

static void alert(int port, int mask)
{
	/* Always update the Alert status register */
	pd[port].alert |= mask;
	/*
	 * Only send interrupt to TCPM if corresponding
	 * bit in the alert_enable register is set.
	 */
	if (pd[port].alert_mask & mask)
		tcpc_alert(port);
}

int tcpc_run(int port, int evt)
{
	int cc, i, res;

	/* Don't do anything when port is not available */
	if (port >= board_get_usb_pd_port_count())
		return -1;

	/* incoming packet ? */
	if (pd_rx_started(port) && pd[port].rx_enabled) {
		/* Get message and place at RX buffer head */
		res = pd[port].rx_head[pd[port].rx_buf_head] = pd_analyze_rx(
			port, pd[port].rx_payload[pd[port].rx_buf_head]);
		pd_rx_complete(port);

		/*
		 * If there is space in buffer, then increment head to keep
		 * the message and send goodCRC. If this is a hard reset,
		 * send alert regardless of rx buffer status. Else if there is
		 * no space in buffer, then do not send goodCRC and drop
		 * message.
		 */
		if (res > 0 && !rx_buf_is_full(port)) {
			rx_buf_increment(port, &pd[port].rx_buf_head);
			handle_request(port, res);
			alert(port, TCPC_REG_ALERT_RX_STATUS);
		} else if (res == PD_RX_ERR_HARD_RESET) {
			alert(port, TCPC_REG_ALERT_RX_HARD_RST);
		}
	}

	/* outgoing packet ? */
	if ((evt & PD_EVENT_TX) && pd[port].rx_enabled) {
		switch (pd[port].tx_type) {
		case TCPCI_MSG_SOP:
			res = send_validate_message(port, pd[port].tx_head,
						    pd[port].tx_data);
			break;
		case TCPCI_MSG_TX_BIST_MODE_2:
			bist_mode_2_tx(port);
			res = 0;
			break;
		case TCPCI_MSG_TX_HARD_RESET:
			res = send_hard_reset(port);
			break;
		default:
			res = PD_TX_ERR_DISABLED;
			break;
		}

		/* send appropriate alert for tx completion */
		if (res >= 0)
			alert(port, TCPC_REG_ALERT_TX_SUCCESS);
		else if (res == PD_TX_ERR_GOODCRC)
			alert(port, TCPC_REG_ALERT_TX_FAILED);
		else
			alert(port, TCPC_REG_ALERT_TX_DISCARDED);
	} else {
		/* If we have nothing to transmit, then sample CC lines */

		/* CC pull changed, wait 1ms for CC voltage to stabilize */
		if (evt & PD_EVENT_CC)
			usleep(MSEC);

		/* check CC lines */
		for (i = 0; i < 2; i++) {
			/* read CC voltage */
			cc = pd_adc_read(port, i);

			/* convert voltage to status, and check status change */
			cc = cc_voltage_to_status(port, cc, i);
			if (pd[port].cc_status[i] != cc) {
				pd[port].cc_status[i] = cc;
				alert(port, TCPC_REG_ALERT_CC_STATUS);
			}
		}
	}

	/* make sure PD monitoring is enabled to wake on PD RX */
	if (pd[port].rx_enabled)
		pd_rx_enable_monitoring(port);

	return 10 * MSEC;
}


void pd_rx_event(int port)
{
	task_set_event(PD_PORT_TO_TASK_ID(port), TASK_EVENT_WAKE);
}

int tcpc_alert_status(int port, int *alert)
{
	/* return the value of the TCPC Alert register */
	uint16_t ret = pd[port].alert;
	*alert = ret;
	return EC_SUCCESS;
}

int tcpc_alert_status_clear(int port, uint16_t mask)
{
	/*
	 * If the RX status alert is attempting to be cleared, then increment
	 * rx buffer tail pointer. if the RX buffer is not empty, then keep
	 * the RX status alert active.
	 */
	if (mask & TCPC_REG_ALERT_RX_STATUS) {
		if (!rx_buf_is_empty(port)) {
			rx_buf_increment(port, &pd[port].rx_buf_tail);
			if (!rx_buf_is_empty(port))
				/* buffer is not empty, keep alert active */
				mask &= ~TCPC_REG_ALERT_RX_STATUS;
		}
	}

	/* clear only the bits specified by the TCPM */
	pd[port].alert &= ~mask;
	return EC_SUCCESS;
}

int tcpc_alert_mask_set(int port, uint16_t mask)
{
	/* Update the alert mask as specificied by the TCPM */
	pd[port].alert_mask = mask;
	return EC_SUCCESS;
}

int tcpc_set_cc(int port, int pull)
{
	/* If CC pull resistor not changing, then nothing to do */
	if (pd[port].cc_pull == pull)
		return EC_SUCCESS;

	/* Change CC pull resistor */
	pd[port].cc_pull = pull;


	/*
	 * Before CC pull can be changed and the task can read the new
	 * status, we should set the CC status to open, in case TCPM
	 * asks before it is known for sure.
	 */
	pd[port].cc_status[0] = TYPEC_CC_VOLT_OPEN;
	pd[port].cc_status[1] = pd[port].cc_status[0];

	/* Wake the PD phy task with special CC event mask */
	/* TODO: use top case if no TCPM on same CPU */
	tcpc_run(port, PD_EVENT_CC);
	return EC_SUCCESS;
}

int tcpc_get_cc(int port, enum tcpc_cc_voltage_status *cc1,
		enum tcpc_cc_voltage_status *cc2)
{
	*cc2 = pd[port].cc_status[1];
	*cc1 = pd[port].cc_status[0];

	return EC_SUCCESS;
}

int board_select_rp_value(int port, int rp) __attribute__((weak));

int tcpc_select_rp_value(int port, int rp)
{
	if (board_select_rp_value)
		return board_select_rp_value(port, rp);
	else
		return EC_ERROR_UNIMPLEMENTED;
}

int tcpc_set_polarity(int port, int polarity)
{
	pd[port].polarity = polarity;
	pd_select_polarity(port, pd[port].polarity);

	return EC_SUCCESS;
}


int tcpc_set_power_status_mask(int port, uint8_t mask)
{
	pd[port].power_status_mask = mask;
	return EC_SUCCESS;
}

int tcpc_set_vconn(int port, int enable)
{
	return EC_SUCCESS;
}

int tcpc_set_rx_enable(int port, int enable)
{
	pd[port].rx_enabled = enable;

	if (!enable)
		pd_rx_disable_monitoring(port);

	return EC_SUCCESS;
}

int tcpc_transmit(int port, enum tcpci_msg_type type, uint16_t header,
		  const uint32_t *data)
{
	/* Store data to transmit and wake task to send it */
	pd[port].tx_type = type;
	pd[port].tx_head = header;
	pd[port].tx_data = data;
	/* TODO: use top case if no TCPM on same CPU */
	tcpc_run(port, PD_EVENT_TX);
	return EC_SUCCESS;
}

int tcpc_set_msg_header(int port, int power_role, int data_role)
{
	pd[port].power_role = power_role;
	pd[port].data_role = data_role;

	return EC_SUCCESS;
}

int tcpc_get_message(int port, uint32_t *payload, int *head)
{
	/* Get message at tail of RX buffer */
	int idx = pd[port].rx_buf_tail;

	memcpy(payload, pd[port].rx_payload[idx],
	       sizeof(pd[port].rx_payload[idx]));
	*head = pd[port].rx_head[idx];
	return EC_SUCCESS;
}

void tcpc_pre_init(void)
{
	int i;

	/* Mark as uninitialized */
	for (i = 0; i < board_get_usb_pd_port_count(); i++)
		pd[i].power_status |= TCPC_REG_POWER_STATUS_UNINIT |
				      TCPC_REG_POWER_STATUS_VBUS_DET;
}
/* Must be prioritized above i2c init */
DECLARE_HOOK(HOOK_INIT, tcpc_pre_init, HOOK_PRIO_INIT_I2C - 1);

void tcpc_init(int port)
{
	int i;

	if (port >= board_get_usb_pd_port_count())
		return;

	/* Initialize physical layer */
	pd_hw_init(port, PD_ROLE_DEFAULT(port));
	pd[port].cc_pull = PD_ROLE_DEFAULT(port) == PD_ROLE_SOURCE ?
				   TYPEC_CC_RP :
				   TYPEC_CC_RD;

	/* make sure PD monitoring is disabled initially */
	pd[port].rx_enabled = 0;

	/* make initial readings of CC voltages */
	for (i = 0; i < 2; i++) {
		pd[port].cc_status[i] =
			cc_voltage_to_status(port, pd_adc_read(port, i), i);
	}


	/* set default alert and power mask register values */
	pd[port].alert_mask = TCPC_REG_ALERT_MASK_ALL;
	pd[port].power_status_mask = TCPC_REG_POWER_STATUS_MASK_ALL;

	/* set power status alert since the UNINIT bit has been set */
	alert(port, TCPC_REG_ALERT_POWER_STATUS);
}



