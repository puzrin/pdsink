/* Copyright 2015 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Author: Gabe Noblesmith
 */
#include <stdatomic.h>
#include <stdbool.h>
#include "usb_pd.h"
#include "usb_pd_tcpm.h"
#include "src/driver/fusb302.h"
#include "timer.h"
#include "src/pd_config.h"
#include "src/portage/fusb302_i2c_drv.h"

// TODO: care about initialization & `on_complete` setup.
static fusb302_i2c_drv_t i2c_drv;

// TODO: fix memory use of `PT_NWAIT` hashtable.
#include "src/pt/protothread.h"
#include "src/pt/protothread_sem.h"

static atomic_flag is_running = ATOMIC_FLAG_INIT;
static atomic_flag deferred_call = ATOMIC_FLAG_INIT;
static state_t pt_scheduler_state = {0};

void pt_schedule() {
	bool should_run = false;

	do {
		if (atomic_flag_test_and_set(&is_running)) {
			atomic_flag_test_and_set(&deferred_call);
			return;
		}

		protothread_run(pt_scheduler_state);
		// Dirty hack to to fetch missed state changes. Probably not required.
		protothread_run(pt_scheduler_state);
		protothread_run(pt_scheduler_state);

		atomic_flag_clear(&is_running);

		if (atomic_flag_test_and_set(&deferred_call)) {
			atomic_flag_clear(&deferred_call);
			should_run = true;
		} else should_run = false;
	} while (should_run);
}

/*
 * Helper to wait for bool condition. Difference with `pt_wait()`:
 * - Wait for simple boolean condition, instead of event
 * - Thread does not sleep.
 */
#define pt_wait_cond(ctx, cond) while (!(cond)) { pt_yield(ctx); }

/******************************************************************************/

/*
 *
 * tcpc methods, adopted to protothreads
 *
 */

// TODO: pin to driver
static void i2c_on_complete() { pt_schedule(); }

// Since tcpc methods are not nested, use shared context for simplicity.
typedef struct {
	pt_thread_t pt_thread;
	pt_func_t pt_func;
} pt_base_ctx_t;

pt_base_ctx_t tcpc_ctx = {0};


static pt_t tcpc_read(void const *env, int reg, int *val) {
	pt_base_ctx_t *const ctx = env;
	pt_resume(ctx);

	i2c_drv.read(/* TODO: fixme */);
	pt_wait_cond(ctx, i2c_drv.done());
	return PT_DONE;
}

static pt_t tcpc_write(void const *env, int reg, int val) {
	pt_base_ctx_t *const ctx = env;
	pt_resume(ctx);

	i2c_drv.write(/* TODO: fixme */);
	pt_wait_cond(ctx, i2c_drv.done());
	return PT_DONE;
}

// Sync methods, for init only, to simplify porting.
static void tcpc_read_sync(int reg, int *val) {
	i2c_drv.read(/* TODO: fixme */);
	while (!i2c_drv.done()) delay(1);
}
void tcpc_write_sync(int reg, int val) {
	i2c_drv.write(/* TODO: fixme */);
	while (!i2c_drv.done()) delay(1);
}

// Value of interrupt pin.
static bool tcpc_has_alert() {
	// TODO: fixme
}

/*
 * Guards to prohibit nested driver calls.
 *
 * NOTE:
 *
 * 1. This does not help with nesting the same method, but such restriction
 *    looks acceptable.
 * 2. This is invoked from flat event loop, atomic operations seems not required.
 */
volatile bool _tcpc_lock_flag = false;
#define tcpc_lock(ctx) pt_wait_cond(ctx, !_tcpc_lock_flag); _tcpc_lock_flag = true;
#define tcpc_unlock(ctx) _tcpc_lock_flag = false; pt_schedule();

/******************************************************************************/

#define PACKET_IS_GOOD_CRC(head) \
	(PD_HEADER_TYPE(head) == PD_CTRL_GOOD_CRC && PD_HEADER_CNT(head) == 0)

static struct fusb302_chip_state {
	int cc_polarity;
	int vconn_enabled;
	/* 1 = pulling up (DFP) 0 = pulling down (UFP) */
	int pulling_up;
	int rx_enable;
	uint8_t mdac_vnc;
	uint8_t mdac_rd;
} state[CONFIG_USB_PD_PORT_MAX_COUNT];

/*
 * Bring the FUSB302 out of reset after Hard Reset signaling. This will
 * automatically flush both the Rx and Tx FIFOs.
 */
/*static void fusb302_pd_reset(int port)
{
	tcpc_write(port, TCPC_REG_RESET, TCPC_REG_RESET_PD_RESET);
}*/

/*
 * Flush our Rx FIFO. To prevent packet framing issues, this function should
 * only be called when Rx is disabled.
 */
//static void fusb302_flush_rx_fifo(int port)
//{
	/*
	 * other bits in the register _should_ be 0
	 * until the day we support other SOP* types...
	 * then we'll have to keep a shadow of what this register
	 * value should be so we don't clobber it here!
	 */
//	tcpc_write(port, TCPC_REG_CONTROL1, TCPC_REG_CONTROL1_RX_FLUSH);
//}

/*static void fusb302_flush_tx_fifo(int port)
{
	int reg;

	tcpc_read(port, TCPC_REG_CONTROL0, &reg);
	reg |= TCPC_REG_CONTROL0_TX_FLUSH;
	tcpc_write(port, TCPC_REG_CONTROL0, reg);
}*/

/*static void fusb302_auto_goodcrc_enable(int port, int enable)
{
	int reg;

	tcpc_read(port, TCPC_REG_SWITCHES1, &reg);

	if (enable)
		reg |= TCPC_REG_SWITCHES1_AUTO_GCRC;
	else
		reg &= ~TCPC_REG_SWITCHES1_AUTO_GCRC;

	tcpc_write(port, TCPC_REG_SWITCHES1, reg);
}*/

/* Convert BC LVL values (in FUSB302) to Type-C CC Voltage Status */
static int convert_bc_lvl(int port, int bc_lvl)
{
	/* assume OPEN unless one of the following conditions is true... */
	int ret = TYPEC_CC_VOLT_OPEN;

	if (state[port].pulling_up) {
		if (bc_lvl == 0x00)
			ret = TYPEC_CC_VOLT_RA;
		else if (bc_lvl < 0x3)
			ret = TYPEC_CC_VOLT_RD;
	} else {
		if (bc_lvl == 0x1)
			ret = TYPEC_CC_VOLT_RP_DEF;
		else if (bc_lvl == 0x2)
			ret = TYPEC_CC_VOLT_RP_1_5;
		else if (bc_lvl == 0x3)
			ret = TYPEC_CC_VOLT_RP_3_0;
	}

	return ret;
}

/* Determine cc pin state for sink */
static void detect_cc_pin_sink(int port, enum tcpc_cc_voltage_status *cc1,
			       enum tcpc_cc_voltage_status *cc2)
{
	int reg;
	int orig_meas_cc1;
	int orig_meas_cc2;
	int bc_lvl_cc1;
	int bc_lvl_cc2;

	mutex_lock(&measure_lock);

	/*
	 * Measure CC1 first.
	 */
	tcpc_read(port, TCPC_REG_SWITCHES0, &reg);

	/* save original state to be returned to later... */
	if (reg & TCPC_REG_SWITCHES0_MEAS_CC1)
		orig_meas_cc1 = 1;
	else
		orig_meas_cc1 = 0;

	if (reg & TCPC_REG_SWITCHES0_MEAS_CC2)
		orig_meas_cc2 = 1;
	else
		orig_meas_cc2 = 0;

	/* Disable CC2 measurement switch, enable CC1 measurement switch */
	reg &= ~TCPC_REG_SWITCHES0_MEAS_CC2;
	reg |= TCPC_REG_SWITCHES0_MEAS_CC1;

	tcpc_write(port, TCPC_REG_SWITCHES0, reg);

	/* CC1 is now being measured by FUSB302. */

	/* Wait on measurement */
	usleep(250);

	tcpc_read(port, TCPC_REG_STATUS0, &bc_lvl_cc1);

	/* mask away unwanted bits */
	bc_lvl_cc1 &= (TCPC_REG_STATUS0_BC_LVL0 | TCPC_REG_STATUS0_BC_LVL1);

	/*
	 * Measure CC2 next.
	 */

	tcpc_read(port, TCPC_REG_SWITCHES0, &reg);

	/* Disable CC1 measurement switch, enable CC2 measurement switch */
	reg &= ~TCPC_REG_SWITCHES0_MEAS_CC1;
	reg |= TCPC_REG_SWITCHES0_MEAS_CC2;

	tcpc_write(port, TCPC_REG_SWITCHES0, reg);

	/* CC2 is now being measured by FUSB302. */

	/* Wait on measurement */
	usleep(250);

	tcpc_read(port, TCPC_REG_STATUS0, &bc_lvl_cc2);

	/* mask away unwanted bits */
	bc_lvl_cc2 &= (TCPC_REG_STATUS0_BC_LVL0 | TCPC_REG_STATUS0_BC_LVL1);

	*cc1 = convert_bc_lvl(port, bc_lvl_cc1);
	*cc2 = convert_bc_lvl(port, bc_lvl_cc2);

	/* return MEAS_CC1/2 switches to original state */
	tcpc_read(port, TCPC_REG_SWITCHES0, &reg);
	if (orig_meas_cc1)
		reg |= TCPC_REG_SWITCHES0_MEAS_CC1;
	else
		reg &= ~TCPC_REG_SWITCHES0_MEAS_CC1;
	if (orig_meas_cc2)
		reg |= TCPC_REG_SWITCHES0_MEAS_CC2;
	else
		reg &= ~TCPC_REG_SWITCHES0_MEAS_CC2;

	tcpc_write(port, TCPC_REG_SWITCHES0, reg);

	mutex_unlock(&measure_lock);
}

/* Parse header bytes for the size of packet */
static int get_num_bytes(uint16_t header)
{
	int rv;

	/* Grab the Number of Data Objects field.*/
	rv = PD_HEADER_CNT(header);

	/* Multiply by four to go from 32-bit words -> bytes */
	rv *= 4;

	/* Plus 2 for header */
	rv += 2;

	return rv;
}

static int fusb302_send_message(int port, uint16_t header, const uint32_t *data,
				uint8_t *buf, int buf_pos)
{
	int rv;
	int reg;
	int len;

	len = get_num_bytes(header);

	/*
	 * packsym tells the TXFIFO that the next X bytes are payload,
	 * and should not be interpreted as special tokens.
	 * The 5 LSBs represent X, the number of bytes.
	 */
	reg = FUSB302_TKN_PACKSYM;
	reg |= (len & 0x1F);

	buf[buf_pos++] = reg;

	/* write in the header */
	reg = header;
	buf[buf_pos++] = reg & 0xFF;

	reg >>= 8;
	buf[buf_pos++] = reg & 0xFF;

	/* header is done, subtract from length to make this for-loop simpler */
	len -= 2;

	/* write data objects, if present */
	memcpy(&buf[buf_pos], data, len);
	buf_pos += len;

	/* put in the CRC */
	buf[buf_pos++] = FUSB302_TKN_JAMCRC;

	/* put in EOP */
	buf[buf_pos++] = FUSB302_TKN_EOP;

	/* Turn transmitter off after sending message */
	buf[buf_pos++] = FUSB302_TKN_TXOFF;

	/* Start transmission */
	reg = FUSB302_TKN_TXON;
	buf[buf_pos++] = FUSB302_TKN_TXON;

	/* burst write for speed! */
	rv = tcpc_xfer(port, buf, buf_pos, 0, 0);

	return rv;
}

static int fusb302_tcpm_init(int port)
{
	int reg;

	/* set default */
	state[port].cc_polarity = -1;

	/* set the voltage threshold for no connect detection (vOpen) */
	state[port].mdac_vnc = TCPC_REG_MEASURE_MDAC_MV(PD_SRC_DEF_VNC_MV);
	/* set the voltage threshold for Rd vs Ra detection */
	state[port].mdac_rd = TCPC_REG_MEASURE_MDAC_MV(PD_SRC_DEF_RD_THRESH_MV);

	/* all other variables assumed to default to 0 */

	/* Restore default settings */
	tcpc_write(port, TCPC_REG_RESET, TCPC_REG_RESET_SW_RESET);

	/* Turn on retries and set number of retries */
	tcpc_read(port, TCPC_REG_CONTROL3, &reg);
	reg |= TCPC_REG_CONTROL3_AUTO_RETRY;
	reg |= (CONFIG_PD_RETRY_COUNT & 0x3) << TCPC_REG_CONTROL3_N_RETRIES_POS;
	tcpc_write(port, TCPC_REG_CONTROL3, reg);

	/* Create interrupt masks */
	reg = 0xFF;
	/* CC level changes */
	reg &= ~TCPC_REG_MASK_BC_LVL;
	/* collisions */
	reg &= ~TCPC_REG_MASK_COLLISION;
	/* misc alert */
	reg &= ~TCPC_REG_MASK_ALERT;
	tcpc_write(port, TCPC_REG_MASK, reg);

	reg = 0xFF;
	/* when all pd message retries fail... */
	reg &= ~TCPC_REG_MASKA_RETRYFAIL;
	/* when fusb302 send a hard reset. */
	reg &= ~TCPC_REG_MASKA_HARDSENT;
	/* when fusb302 receives GoodCRC ack for a pd message */
	reg &= ~TCPC_REG_MASKA_TX_SUCCESS;
	/* when fusb302 receives a hard reset */
	reg &= ~TCPC_REG_MASKA_HARDRESET;
	tcpc_write(port, TCPC_REG_MASKA, reg);

	reg = 0xFF;
	/* when fusb302 sends GoodCRC to ack a pd message */
	reg &= ~TCPC_REG_MASKB_GCRCSENT;
	tcpc_write(port, TCPC_REG_MASKB, reg);

	/* Interrupt Enable */
	tcpc_read(port, TCPC_REG_CONTROL0, &reg);
	reg &= ~TCPC_REG_CONTROL0_INT_MASK;
	tcpc_write(port, TCPC_REG_CONTROL0, reg);

	/* Set VCONN switch defaults */
	tcpm_set_polarity(port, 0);
	tcpm_set_vconn(port, 0);

	/* TODO: Reduce power consumption */
	tcpc_write(port, TCPC_REG_POWER, TCPC_REG_POWER_PWR_ALL);


	return 0;
}

static int fusb302_tcpm_get_cc(int port, enum tcpc_cc_voltage_status *cc1,
			       enum tcpc_cc_voltage_status *cc2)
{
	if (state[port].pulling_up) {
		/* Source mode? */
		assert(0);/* [hide to reduce code size] detect_cc_pin_source_manual(port, cc1, cc2); */
	} else {
		/* Sink mode? */
		detect_cc_pin_sink(port, cc1, cc2);
	}

	return 0;
}

static int fusb302_tcpm_set_cc(int port, int pull)
{
	int reg;

	/* NOTE: FUSB302 toggles a single pull-up between CC1 and CC2 */
	/* NOTE: FUSB302 Does not support Ra. */
	switch (pull) {
	case TYPEC_CC_RP:
		/* enable the pull-up we know to be necessary */
		tcpc_read(port, TCPC_REG_SWITCHES0, &reg);

		reg &= ~(TCPC_REG_SWITCHES0_CC2_PU_EN |
			 TCPC_REG_SWITCHES0_CC1_PU_EN |
			 TCPC_REG_SWITCHES0_CC1_PD_EN |
			 TCPC_REG_SWITCHES0_CC2_PD_EN |
			 TCPC_REG_SWITCHES0_VCONN_CC1 |
			 TCPC_REG_SWITCHES0_VCONN_CC2);

		reg |= TCPC_REG_SWITCHES0_CC1_PU_EN |
		       TCPC_REG_SWITCHES0_CC2_PU_EN;

		if (state[port].vconn_enabled)
			reg |= state[port].cc_polarity ?
				       TCPC_REG_SWITCHES0_VCONN_CC1 :
				       TCPC_REG_SWITCHES0_VCONN_CC2;

		tcpc_write(port, TCPC_REG_SWITCHES0, reg);

		state[port].pulling_up = 1;
		break;
	case TYPEC_CC_RD:
		/* Enable UFP Mode */

		/* turn off toggle */
		tcpc_read(port, TCPC_REG_CONTROL2, &reg);
		reg &= ~TCPC_REG_CONTROL2_TOGGLE;
		tcpc_write(port, TCPC_REG_CONTROL2, reg);

		/* enable pull-downs, disable pullups */
		tcpc_read(port, TCPC_REG_SWITCHES0, &reg);

		reg &= ~(TCPC_REG_SWITCHES0_CC2_PU_EN);
		reg &= ~(TCPC_REG_SWITCHES0_CC1_PU_EN);
		reg |= (TCPC_REG_SWITCHES0_CC1_PD_EN);
		reg |= (TCPC_REG_SWITCHES0_CC2_PD_EN);
		tcpc_write(port, TCPC_REG_SWITCHES0, reg);

		state[port].pulling_up = 0;
		break;
	case TYPEC_CC_OPEN:
		/* Disable toggling */
		tcpc_read(port, TCPC_REG_CONTROL2, &reg);
		reg &= ~TCPC_REG_CONTROL2_TOGGLE;
		tcpc_write(port, TCPC_REG_CONTROL2, reg);

		/* Ensure manual switches are opened */
		tcpc_read(port, TCPC_REG_SWITCHES0, &reg);
		reg &= ~TCPC_REG_SWITCHES0_CC1_PU_EN;
		reg &= ~TCPC_REG_SWITCHES0_CC2_PU_EN;
		reg &= ~TCPC_REG_SWITCHES0_CC1_PD_EN;
		reg &= ~TCPC_REG_SWITCHES0_CC2_PD_EN;
		tcpc_write(port, TCPC_REG_SWITCHES0, reg);

		state[port].pulling_up = 0;
		break;
	default:
		/* Unsupported... */
		return EC_ERROR_UNIMPLEMENTED;
	}
	return 0;
}

static int fusb302_tcpm_set_polarity(int port, enum tcpc_cc_polarity polarity)
{
	/* Port polarity : 0 => CC1 is CC line, 1 => CC2 is CC line */
	int reg;

	tcpc_read(port, TCPC_REG_SWITCHES0, &reg);

	/* clear VCONN switch bits */
	reg &= ~TCPC_REG_SWITCHES0_VCONN_CC1;
	reg &= ~TCPC_REG_SWITCHES0_VCONN_CC2;

	if (state[port].vconn_enabled) {
		/* set VCONN switch to be non-CC line */
		if (polarity_rm_dts(polarity))
			reg |= TCPC_REG_SWITCHES0_VCONN_CC1;
		else
			reg |= TCPC_REG_SWITCHES0_VCONN_CC2;
	}

	/* clear meas_cc bits (RX line select) */
	reg &= ~TCPC_REG_SWITCHES0_MEAS_CC1;
	reg &= ~TCPC_REG_SWITCHES0_MEAS_CC2;

	/* set rx polarity */
	if (polarity_rm_dts(polarity))
		reg |= TCPC_REG_SWITCHES0_MEAS_CC2;
	else
		reg |= TCPC_REG_SWITCHES0_MEAS_CC1;

	tcpc_write(port, TCPC_REG_SWITCHES0, reg);

	tcpc_read(port, TCPC_REG_SWITCHES1, &reg);

	/* clear tx_cc bits */
	reg &= ~TCPC_REG_SWITCHES1_TXCC1_EN;
	reg &= ~TCPC_REG_SWITCHES1_TXCC2_EN;

	/* set tx polarity */
	if (polarity_rm_dts(polarity))
		reg |= TCPC_REG_SWITCHES1_TXCC2_EN;
	else
		reg |= TCPC_REG_SWITCHES1_TXCC1_EN;

	tcpc_write(port, TCPC_REG_SWITCHES1, reg);

	/* Save the polarity for later */
	state[port].cc_polarity = polarity;

	return 0;
}



typedef struct {
	pt_thread_t pt_thread;
	pt_func_t pt_func;
	int port;
	int power_role;
	int data_role;
} set_msg_header_ctx_t;

// Use static alloc, since nested calls not allowed.
static set_msg_header_ctx_t set_msg_header_ctx = {0};

static int fusb302_tcpm_set_msg_header(int port, int power_role, int data_role) {
	// Move params to thread context struct
	set_msg_header_ctx.port = port;
	set_msg_header_ctx.power_role = power_role;
	set_msg_header_ctx.data_role = data_role;

	pt_create(
		&pt_scheduler_state,
		&set_msg_header_ctx.pt_thread,
		fusb302_tcpm_set_msg_header_pt,
		&set_msg_header_ctx
	);

	pt_schedule();

	return 0;
}

static pt_t fusb302_tcpm_set_msg_header_pt(void * const env)
{
	// Use static to keep var value between calls. Dirty but simple.
	static int reg;

	set_msg_header_ctx_t *const ctx = env;
	pt_resume(ctx);

	tcpc_lock(ctx);

	pt_call(ctx, tcpc_read, &tcpc_ctx, TCPC_REG_SWITCHES1, &reg);

	reg &= ~TCPC_REG_SWITCHES1_POWERROLE;
	reg &= ~TCPC_REG_SWITCHES1_DATAROLE;

	if (ctx->power_role)
		reg |= TCPC_REG_SWITCHES1_POWERROLE;
	if (ctx->data_role)
		reg |= TCPC_REG_SWITCHES1_DATAROLE;

	pt_call(ctx, tcpc_write, &tcpc_ctx, TCPC_REG_SWITCHES1, reg);

	tcpc_unlock(ctx);

	return PT_DONE;
}

static int fusb302_tcpm_set_rx_enable(int port, int enable)
{
	int reg;

	state[port].rx_enable = enable;

	/* Get current switch state */
	tcpc_read(port, TCPC_REG_SWITCHES0, &reg);

	/* Clear CC1/CC2 measure bits */
	reg &= ~TCPC_REG_SWITCHES0_MEAS_CC1;
	reg &= ~TCPC_REG_SWITCHES0_MEAS_CC2;

	if (enable) {
		switch (state[port].cc_polarity) {
		/* if CC polarity hasnt been determined, can't enable */
		case -1:
			return EC_ERROR_UNKNOWN;
		case 0:
			reg |= TCPC_REG_SWITCHES0_MEAS_CC1;
			break;
		case 1:
			reg |= TCPC_REG_SWITCHES0_MEAS_CC2;
			break;
		default:
			/* "shouldn't get here" */
			return EC_ERROR_UNKNOWN;
		}
		tcpc_write(port, TCPC_REG_SWITCHES0, reg);

		/* Disable BC_LVL interrupt when enabling PD comm */
		if (!tcpc_read(port, TCPC_REG_MASK, &reg))
			tcpc_write(port, TCPC_REG_MASK,
				   reg | TCPC_REG_MASK_BC_LVL);

		/* flush rx fifo in case messages have been coming our way */
		/*fusb302_flush_rx_fifo(port);*/
		tcpc_write(port, TCPC_REG_CONTROL1, TCPC_REG_CONTROL1_RX_FLUSH);

	} else {
		tcpc_write(port, TCPC_REG_SWITCHES0, reg);

		/* Enable BC_LVL interrupt when disabling PD comm */
		if (!tcpc_read(port, TCPC_REG_MASK, &reg))
			tcpc_write(port, TCPC_REG_MASK,
				   reg & ~TCPC_REG_MASK_BC_LVL);
	}


	/*fusb302_auto_goodcrc_enable(port, enable);*/
	tcpc_read(port, TCPC_REG_SWITCHES1, &reg);
	if (enable) reg |= TCPC_REG_SWITCHES1_AUTO_GCRC;
	else reg &= ~TCPC_REG_SWITCHES1_AUTO_GCRC;
	tcpc_write(port, TCPC_REG_SWITCHES1, reg);

	return 0;
}

/* Return true if our Rx FIFO is empty */
static int fusb302_rx_fifo_is_empty(int port)
{
	int reg;

	return (!tcpc_read(port, TCPC_REG_STATUS1, &reg)) &&
	       (reg & TCPC_REG_STATUS1_RX_EMPTY);
}

static int fusb302_tcpm_get_message_raw(int port, uint32_t *payload, int *head)
{
	/*
	 * This is the buffer that will get the burst-read data
	 * from the fusb302.
	 *
	 * It's re-used in a couple different spots, the worst of which
	 * is the PD packet (not header) and CRC.
	 * maximum size necessary = 28 + 4 = 32
	 */
	uint8_t buf[32];
	int rv, len;

	/* Read until we have a non-GoodCRC packet or an empty FIFO */
	do {
		buf[0] = TCPC_REG_FIFOS;
		tcpc_lock(port, 1);

		/*
		 * PART 1 OF BURST READ: Write in register address.
		 * Issue a START, no STOP.
		 */
		rv = tcpc_xfer_unlocked(port, buf, 1, 0, 0, I2C_XFER_START);

		/*
		 * PART 2 OF BURST READ: Read up to the header.
		 * Issue a repeated START, no STOP.
		 * only grab three bytes so we can get the header
		 * and determine how many more bytes we need to read.
		 * TODO: Check token to ensure valid packet.
		 */
		rv |= tcpc_xfer_unlocked(port, 0, 0, buf, 3, I2C_XFER_START);

		/* Grab the header */
		*head = (buf[1] & 0xFF);
		*head |= ((buf[2] << 8) & 0xFF00);

		/* figure out packet length, subtract header bytes */
		len = get_num_bytes(*head) - 2;

		/*
		 * PART 3 OF BURST READ: Read everything else.
		 * No START, but do issue a STOP at the end.
		 * add 4 to len to read CRC out
		 */
		rv |= tcpc_xfer_unlocked(port, 0, 0, buf, len + 4,
					 I2C_XFER_STOP);

		tcpc_lock(port, 0);
	} while (!rv && PACKET_IS_GOOD_CRC(*head) &&
		 !fusb302_rx_fifo_is_empty(port));

	if (!rv) {
		/* Discard GoodCRC packets */
		if (PACKET_IS_GOOD_CRC(*head))
			rv = EC_ERROR_UNKNOWN;
		else
			memcpy(payload, buf, len);
	}


	return rv;
}

static int fusb302_tcpm_transmit(int port, enum tcpci_msg_type type,
				 uint16_t header, const uint32_t *data)
{
	/*
	 * this is the buffer that will be burst-written into the fusb302
	 * maximum size necessary =
	 * 1: FIFO register address
	 * 4: SOP* tokens
	 * 1: Token that signifies "next X bytes are not tokens"
	 * 30: 2 for header and up to 7*4 = 28 for rest of message
	 * 1: "Insert CRC" Token
	 * 1: EOP Token
	 * 1: "Turn transmitter off" token
	 * 1: "Star Transmission" Command
	 * -
	 * 40: 40 bytes worst-case
	 */
	uint8_t buf[40];
	int buf_pos = 0;

	int reg;

	/* Flush the TXFIFO */
	/*fusb302_flush_tx_fifo(port);*/
	tcpc_read(port, TCPC_REG_CONTROL0, &reg);
	reg |= TCPC_REG_CONTROL0_TX_FLUSH;
	tcpc_write(port, TCPC_REG_CONTROL0, reg);


	switch (type) {
	case TCPCI_MSG_SOP:

		/* put register address first for of burst tcpc write */
		buf[buf_pos++] = TCPC_REG_FIFOS;

		/* Write the SOP Ordered Set into TX FIFO */
		buf[buf_pos++] = FUSB302_TKN_SYNC1;
		buf[buf_pos++] = FUSB302_TKN_SYNC1;
		buf[buf_pos++] = FUSB302_TKN_SYNC1;
		buf[buf_pos++] = FUSB302_TKN_SYNC2;

		return fusb302_send_message(port, header, data, buf, buf_pos);
	case TCPCI_MSG_SOP_PRIME:

		/* put register address first for of burst tcpc write */
		buf[buf_pos++] = TCPC_REG_FIFOS;

		/* Write the SOP' Ordered Set into TX FIFO */
		buf[buf_pos++] = FUSB302_TKN_SYNC1;
		buf[buf_pos++] = FUSB302_TKN_SYNC1;
		buf[buf_pos++] = FUSB302_TKN_SYNC3;
		buf[buf_pos++] = FUSB302_TKN_SYNC3;

		return fusb302_send_message(port, header, data, buf, buf_pos);
	case TCPCI_MSG_SOP_PRIME_PRIME:

		/* put register address first for of burst tcpc write */
		buf[buf_pos++] = TCPC_REG_FIFOS;

		/* Write the SOP'' Ordered Set into TX FIFO */
		buf[buf_pos++] = FUSB302_TKN_SYNC1;
		buf[buf_pos++] = FUSB302_TKN_SYNC3;
		buf[buf_pos++] = FUSB302_TKN_SYNC1;
		buf[buf_pos++] = FUSB302_TKN_SYNC3;

		return fusb302_send_message(port, header, data, buf, buf_pos);
	case TCPCI_MSG_TX_HARD_RESET:
		/* Simply hit the SEND_HARD_RESET bit */
		tcpc_read(port, TCPC_REG_CONTROL3, &reg);
		reg |= TCPC_REG_CONTROL3_SEND_HARDRESET;
		tcpc_write(port, TCPC_REG_CONTROL3, reg);

		break;
	case TCPCI_MSG_TX_BIST_MODE_2:
		/* Hit the BIST_MODE2 bit and start TX */
		tcpc_read(port, TCPC_REG_CONTROL1, &reg);
		reg |= TCPC_REG_CONTROL1_BIST_MODE2;
		tcpc_write(port, TCPC_REG_CONTROL1, reg);

		tcpc_read(port, TCPC_REG_CONTROL0, &reg);
		reg |= TCPC_REG_CONTROL0_TX_START;
		tcpc_write(port, TCPC_REG_CONTROL0, reg);

		task_wait_event(PD_T_BIST_TRANSMIT);

		/* Clear BIST mode bit, TX_START is self-clearing */
		tcpc_read(port, TCPC_REG_CONTROL1, &reg);
		reg &= ~TCPC_REG_CONTROL1_BIST_MODE2;
		tcpc_write(port, TCPC_REG_CONTROL1, reg);

		break;
	default:
		return EC_ERROR_UNIMPLEMENTED;
	}

	return 0;
}

static bool has_alert = false;

// Interrupt handler.
static void fusb302_tcpc_alert(int port)
{
	// Only signal to thread about data ready
	has_alert = true;
	pt_schedule();
}

// Interrupt data processing thread
static pt_t fusb302_tcpc_alert(void * const env)
{
	static int port = 0; // quick hack
	pt_base_ctx_t *const ctx = env;
	pt_resume(ctx);

	while (1) {
		// Wait for interrupt signal AND (?) check pin for sure
		pt_wait_cond(ctx, has_alert || tcpc_has_alert());
		has_alert = false;

		/* interrupt has been received */
		static int interrupt;
		static int interrupta;
		static int interruptb;

		/* reading interrupt registers clears them */

		pt_call(ctx, tcpc_read, &tcpc_ctx, TCPC_REG_INTERRUPT, &interrupt);
		pt_call(ctx, tcpc_read, &tcpc_ctx, TCPC_REG_INTERRUPTA, &interrupta);
		pt_call(ctx, tcpc_read, &tcpc_ctx, TCPC_REG_INTERRUPTB, &interruptb);

		/*
		* Ignore BC_LVL changes when transmitting / receiving PD,
		* since CC level will constantly change.
		*/
		if (state[port].rx_enable)
			interrupt &= ~TCPC_REG_INTERRUPT_BC_LVL;

		if (interrupt & TCPC_REG_INTERRUPT_BC_LVL) {
			/* CC Status change */
			pd_loop_set_event(port, PD_EVENT_CC);
		}

		if (interrupt & TCPC_REG_INTERRUPT_COLLISION) {
			/* packet sending collided */
			pd_transmit_complete(port, TCPC_TX_COMPLETE_FAILED);
		}


		/* GoodCRC was received, our FIFO is now non-empty */
		if (interrupta & TCPC_REG_INTERRUPTA_TX_SUCCESS) {
			pd_transmit_complete(port, TCPC_TX_COMPLETE_SUCCESS);
		}

		if (interrupta & TCPC_REG_INTERRUPTA_RETRYFAIL) {
			/* all retries have failed to get a GoodCRC */
			pd_transmit_complete(port, TCPC_TX_COMPLETE_FAILED);
		}

		if (interrupta & TCPC_REG_INTERRUPTA_HARDSENT) {
			/* hard reset has been sent */

			/* bring FUSB302 out of reset */
			/*fusb302_pd_reset(port);*/
			pt_call(ctx, tcpc_write, &tcpc_ctx, TCPC_REG_RESET, TCPC_REG_RESET_PD_RESET);
			pd_transmit_complete(port, TCPC_TX_COMPLETE_SUCCESS);
		}

		if (interrupta & TCPC_REG_INTERRUPTA_HARDRESET) {
			/* hard reset has been received */

			/* bring FUSB302 out of reset */
			/*fusb302_pd_reset(port);*/
			pt_call(ctx, tcpc_write, &tcpc_ctx, TCPC_REG_RESET, TCPC_REG_RESET_PD_RESET);
			pd_loop_set_event(port, PD_EVENT_RX_HARD_RESET);
		}

		if (interruptb & TCPC_REG_INTERRUPTB_GCRCSENT) {
			/* Packet received and GoodCRC sent */
			/* (this interrupt fires after the GoodCRC finishes) */
			if (state[port].rx_enable) {
				/* Pull all RX messages from TCPC into EC memory */
				// TODO: fixme
				while (!fusb302_rx_fifo_is_empty(port))
					tcpm_enqueue_message(port);
			} else {
				/* flush rx fifo if rx isn't enabled */
				/*fusb302_flush_rx_fifo(port);*/
				pt_call(ctx, tcpc_write, &tcpc_ctx, TCPC_REG_CONTROL1, TCPC_REG_CONTROL1_RX_FLUSH);
			}
		}

		tcpc_unlock(ctx);
	}
}

const struct tcpm_drv fusb302_tcpm_drv = {
	.init = &fusb302_tcpm_init,
	.release = NULL,
	.get_cc = &fusb302_tcpm_get_cc,
	.get_vbus_voltage = NULL,
	.select_rp_value = NULL,
	.set_cc = &fusb302_tcpm_set_cc,
	.set_polarity = &fusb302_tcpm_set_polarity,
	.set_vconn = NULL,
	.set_msg_header = &fusb302_tcpm_set_msg_header,
	.set_rx_enable = &fusb302_tcpm_set_rx_enable,
	.get_message_raw = &fusb302_tcpm_get_message_raw,
	.transmit = &fusb302_tcpm_transmit,
	.tcpc_alert = &fusb302_tcpc_alert,
};
