/* Copyright 2019 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * TODO(b/272518464): Work around coreboot GCC preprocessor bug.
 * #line marks the *next* line, so it is off by one.
 */
#line 11

#include "atomic.h"
#include "battery.h"
#include "battery_smart.h"
#include "charge_manager.h"
#include "common.h"
#include "dps.h"
#include "driver/tcpm/tcpm.h"
#include "host_command.h"
#include "stdbool.h"
#include "usb_charge.h"
#include "usb_common.h"
#include "usb_emsg.h"
#include "usb_pd.h"
#include "usb_pd_dpm_sm.h"
#include "usb_pd_policy.h"
#include "usb_pd_timer.h"
#include "usb_pe_private.h"
#include "usb_pe_sm.h"
#include "usb_prl_sm.h"
#include "usb_sm.h"
#include "usb_tc_sm.h"
#include "usbc_ppc.h"
#include "util.h"

/*
 * USB Policy Engine Sink / Source module
 *
 * Based on Revision 3.0, Version 1.2 of
 * the USB Power Delivery Specification.
 */

#define CPRINTF(format, args...)
#define CPRINTS(format, args...)

#define CPRINTF_LX(x, format, args...)           \
	do {                                     \
		if (pe_debug_level >= x)         \
			CPRINTF(format, ##args); \
	} while (0)
#define CPRINTF_L1(format, args...) CPRINTF_LX(1, format, ##args)
#define CPRINTF_L2(format, args...) CPRINTF_LX(2, format, ##args)
#define CPRINTF_L3(format, args...) CPRINTF_LX(3, format, ##args)

#define CPRINTS_LX(x, format, args...)           \
	do {                                     \
		if (pe_debug_level >= x)         \
			CPRINTS(format, ##args); \
	} while (0)
#define CPRINTS_L1(format, args...) CPRINTS_LX(1, format, ##args)
#define CPRINTS_L2(format, args...) CPRINTS_LX(2, format, ##args)
#define CPRINTS_L3(format, args...) CPRINTS_LX(3, format, ##args)

#define PE_SET_FN(port, _fn) \
	atomic_or(ATOMIC_ELEM(pe[port].flags_a, (_fn)), ATOMIC_MASK(_fn))
#define PE_CLR_FN(port, _fn)                                    \
	atomic_clear_bits(ATOMIC_ELEM(pe[port].flags_a, (_fn)), \
			  ATOMIC_MASK(_fn))
#define PE_CHK_FN(port, _fn) \
	(pe[port].flags_a[ATOMIC_ELEM(0, (_fn))] & ATOMIC_MASK(_fn))

#define PE_SET_FLAG(port, name) PE_SET_FN(port, (name##_FN))
#define PE_CLR_FLAG(port, name) PE_CLR_FN(port, (name##_FN))
#define PE_CHK_FLAG(port, name) PE_CHK_FN(port, (name##_FN))

/*
 * TODO(b/229655319): support more than 32 bits
 */
#define PE_SET_MASK(port, mask) atomic_or(&pe[port].flags_a[0], (mask))
#define PE_CLR_MASK(port, mask) atomic_clear_bits(&pe[port].flags_a[0], (mask))

/*
 * These macros SET, CLEAR, and CHECK, a DPM (Device Policy Manager)
 * Request. The Requests are listed in usb_pe_sm.h.
 */
#define PE_SET_DPM_REQUEST(port, req) atomic_or(&pe[port].dpm_request, (req))
#define PE_CLR_DPM_REQUEST(port, req) \
	atomic_clear_bits(&pe[port].dpm_request, (req))
#define PE_CHK_DPM_REQUEST(port, req) (pe[port].dpm_request & (req))

/* Message flags which should not persist on returning to ready state */
#define PE_MASK_READY_CLR                         \
	(BIT(PE_FLAGS_LOCALLY_INITIATED_AMS_FN) | \
	 BIT(PE_FLAGS_MSG_DISCARDED_FN) |         \
	 BIT(PE_FLAGS_VDM_REQUEST_TIMEOUT_FN) |   \
	 BIT(PE_FLAGS_INTERRUPTIBLE_AMS_FN))

/*
 * Combination to check whether a reply to a message was received.  Our message
 * should have sent (i.e. not been discarded) and a partner message is ready to
 * process.
 *
 * When chunking is disabled (ex. for PD 2.0), these flags will set
 * on the same run cycle.  With chunking, received message will take an
 * additional cycle to be flagged.
 */
#define PE_CHK_REPLY(port)                           \
	(PE_CHK_FLAG(port, PE_FLAGS_MSG_RECEIVED) && \
	 !PE_CHK_FLAG(port, PE_FLAGS_MSG_DISCARDED))
/* 6.7.3 Hard Reset Counter */
#define N_HARD_RESET_COUNT 2

/* 6.7.4 Capabilities Counter */
#define N_CAPS_COUNT 25

/* 6.7.5 Discover Identity Counter */
/*
 * NOTE: The Protocol Layer tries to send a message 3 time before giving up,
 * so a Discover Identity SOP' message will be sent 3*6 = 18 times (slightly
 * less than spec maximum of 20).  This counter applies only to cable plug
 * discovery.
 */
#define N_DISCOVER_IDENTITY_COUNT 6

/*
 * It is permitted to send SOP' Discover Identity messages before a PD contract
 * is in place. However, this is only beneficial if the cable powers up quickly
 * solely from VCONN. Limit the number of retries without a contract to
 * ensure we attempt some cable discovery after a contract is in place.
 */
#define N_DISCOVER_IDENTITY_PRECONTRACT_LIMIT 2

/*
 * Once this limit of SOP' Discover Identity messages has been set, downgrade
 * to PD 2.0 in case the cable is non-compliant about GoodCRC-ing higher
 * revisions.  This limit should be higher than the precontract limit.
 */
#define N_DISCOVER_IDENTITY_PD3_0_LIMIT 4

/*
 * tDiscoverIdentity is only defined while an explicit contract is in place, so
 * extend the interval between retries pre-contract.
 */
#define PE_T_DISCOVER_IDENTITY_NO_CONTRACT (200 * MSEC)

/*
 * Only VCONN source can communicate with the cable plug. Hence, try VCONN swap
 * 3 times before giving up.
 *
 * Note: This is not a part of power delivery specification
 */
#define N_VCONN_SWAP_COUNT 3

/*
 * Counter to track how many times to attempt SRC to SNK PR swaps before giving
 * up.
 *
 * Note: This is not a part of power delivery specification
 */
#define N_SNK_SRC_PR_SWAP_COUNT 5

/*
 * ChromeOS policy:
 *   For PD2.0, We must be DFP before sending Discover Identity message
 *   to the port partner. Attempt to DR SWAP from UFP to DFP
 *   N_DR_SWAP_ATTEMPT_COUNT times before giving up on sending a
 *   Discover Identity message.
 */
#define N_DR_SWAP_ATTEMPT_COUNT 5

#define TIMER_DISABLED 0xffffffffffffffff /* Unreachable time in future */

/*
 * The time that we allow the port partner to send any messages after an
 * explicit contract is established.  200ms was chosen somewhat arbitrarily as
 * it should be long enough for sources to decide to send a message if they were
 * going to, but not so long that a "low power charger connected" notification
 * would be shown in the chrome OS UI. Setting t0o large a delay can cause
 * problems if the PD discovery time exceeds 1s (tAMETimeout)
 */
#define SRC_SNK_READY_HOLD_OFF_US (200 * MSEC)

/*
 * Function pointer to a Structured Vendor Defined Message (SVDM) response
 * function defined in the board's usb_pd_policy.c file.
 */
typedef int (*svdm_rsp_func)(int port, uint32_t *payload);

/* List of all Policy Engine level states */
enum usb_pe_state {
	/* Super States */
	PE_PRS_FRS_SHARED, /* pe-st0 */
	PE_VDM_SEND_REQUEST, /* pe-st1 */

	/* Normal States */
	PE_SRC_STARTUP, /* pe-st2 */
	PE_SRC_DISCOVERY, /* pe-st3 */
	PE_SRC_SEND_CAPABILITIES, /* pe-st4 */
	PE_SRC_NEGOTIATE_CAPABILITY, /* pe-st5 */
	PE_SRC_TRANSITION_SUPPLY, /* pe-st6 */
	PE_SRC_READY, /* pe-st7 */
	PE_SRC_DISABLED, /* pe-st8 */
	PE_SRC_CAPABILITY_RESPONSE, /* pe-st9 */
	PE_SRC_HARD_RESET, /* pe-st10 */
	PE_SRC_HARD_RESET_RECEIVED, /* pe-st11 */
	PE_SRC_TRANSITION_TO_DEFAULT, /* pe-st12 */
	PE_SNK_STARTUP, /* pe-st13 */
	PE_SNK_DISCOVERY, /* pe-st14 */
	PE_SNK_WAIT_FOR_CAPABILITIES, /* pe-st15 */
	PE_SNK_EVALUATE_CAPABILITY, /* pe-st16 */
	PE_SNK_SELECT_CAPABILITY, /* pe-st17 */
	PE_SNK_READY, /* pe-st18 */
	PE_SNK_HARD_RESET, /* pe-st19 */
	PE_SNK_TRANSITION_TO_DEFAULT, /* pe-st20 */
	PE_SNK_GIVE_SINK_CAP, /* pe-st21 */
	PE_SNK_GET_SOURCE_CAP, /* pe-st22 */
	PE_SNK_TRANSITION_SINK, /* pe-st23 */
	PE_SEND_SOFT_RESET, /* pe-st24 */
	PE_SOFT_RESET, /* pe-st25 */
	PE_SEND_NOT_SUPPORTED, /* pe-st26 */
	PE_SRC_PING, /* pe-st27 */
	PE_DRS_EVALUATE_SWAP, /* pe-st28 */
	PE_DRS_CHANGE, /* pe-st29 */
	PE_DRS_SEND_SWAP, /* pe-st30 */
	PE_PRS_SRC_SNK_EVALUATE_SWAP, /* pe-st31 */
	PE_PRS_SRC_SNK_TRANSITION_TO_OFF, /* pe-st32 */
	PE_PRS_SRC_SNK_ASSERT_RD, /* pe-st33 */
	PE_PRS_SRC_SNK_WAIT_SOURCE_ON, /* pe-st34 */
	PE_PRS_SRC_SNK_SEND_SWAP, /* pe-st35 */
	PE_PRS_SNK_SRC_EVALUATE_SWAP, /* pe-st36 */
	PE_PRS_SNK_SRC_TRANSITION_TO_OFF, /* pe-st37 */
	PE_PRS_SNK_SRC_ASSERT_RP, /* pe-st38 */
	PE_PRS_SNK_SRC_SOURCE_ON, /* pe-st39 */
	PE_PRS_SNK_SRC_SEND_SWAP, /* pe-st40 */
	PE_VCS_EVALUATE_SWAP, /* pe-st41 */
	PE_VCS_SEND_SWAP, /* pe-st42 */
	PE_VCS_WAIT_FOR_VCONN_SWAP, /* pe-st43 */
	PE_VCS_TURN_ON_VCONN_SWAP, /* pe-st44 */
	PE_VCS_TURN_OFF_VCONN_SWAP, /* pe-st45 */
	PE_VCS_SEND_PS_RDY_SWAP, /* pe-st46 */
	PE_VCS_CBL_SEND_SOFT_RESET, /* pe-st47 */
	PE_VDM_IDENTITY_REQUEST_CBL, /* pe-st48 */
	PE_INIT_PORT_VDM_IDENTITY_REQUEST, /* pe-st49 */
	PE_INIT_VDM_SVIDS_REQUEST, /* pe-st50 */
	PE_INIT_VDM_MODES_REQUEST, /* pe-st51 */
	PE_VDM_REQUEST_DPM, /* pe-st52 */
	PE_VDM_RESPONSE, /* pe-st53 */
	PE_WAIT_FOR_ERROR_RECOVERY, /* pe-st54 */
	PE_BIST_TX, /* pe-st55 */
	PE_DEU_SEND_ENTER_USB, /* pe-st56 */
	PE_DR_GET_SINK_CAP, /* pe-st57 */
	PE_DR_SNK_GIVE_SOURCE_CAP, /* pe-st58 */
	PE_DR_SRC_GET_SOURCE_CAP, /* pe-st59 */

	/* PD3.0 only states below here*/
	/* UFP Data Reset States */
	PE_UDR_SEND_DATA_RESET, /* pe-st60 */
	PE_UDR_DATA_RESET_RECEIVED, /* pe-st61 */
	PE_UDR_TURN_OFF_VCONN, /* pe-st62 */
	PE_UDR_SEND_PS_RDY, /* pe-st63 */
	PE_UDR_WAIT_FOR_DATA_RESET_COMPLETE, /* pe-st64 */
	/* DFP Data Reset States */
	PE_DDR_SEND_DATA_RESET, /* pe-st65 */
	PE_DDR_DATA_RESET_RECEIVED, /* pe-st66 */
	PE_DDR_WAIT_FOR_VCONN_OFF, /* pe-st67 */
	PE_DDR_PERFORM_DATA_RESET, /* pe-st68 */
	PE_FRS_SNK_SRC_START_AMS, /* pe-st69 */
	PE_GIVE_BATTERY_CAP, /* pe-st70 */
	PE_GIVE_BATTERY_STATUS, /* pe-st71 */
	PE_GIVE_STATUS, /* pe-st72 */
	PE_SEND_ALERT, /* pe-st73 */
	PE_ALERT_RECEIVED, /* pe-st74 */
	PE_SRC_CHUNK_RECEIVED, /* pe-st75 */
	PE_SNK_CHUNK_RECEIVED, /* pe-st76 */
	PE_VCS_FORCE_VCONN, /* pe-st77 */
	PE_GET_REVISION, /* pe-st78 */

	/* EPR states */
	PE_SNK_SEND_EPR_MODE_ENTRY,
	PE_SNK_EPR_MODE_ENTRY_WAIT_FOR_RESPONSE,
	PE_SNK_EPR_KEEP_ALIVE,
	PE_SNK_SEND_EPR_MODE_EXIT,
	PE_SNK_EPR_MODE_EXIT_RECEIVED,
};

/*
 * The result of a previously sent DPM request; used by PE_VDM_SEND_REQUEST to
 * indicate to child states when they need to handle a response.
 */
enum vdm_response_result {
	/* The parent state is still waiting for a response. */
	VDM_RESULT_WAITING,
	/*
	 * The parent state parsed a message, but there is nothing for the child
	 * to handle, e.g. BUSY.
	 */
	VDM_RESULT_NO_ACTION,
	/* The parent state processed an ACK response. */
	VDM_RESULT_ACK,
	/*
	 * The parent state processed a NAK-like response (NAK, Not Supported,
	 * or response timeout.
	 */
	VDM_RESULT_NAK,
};

/* Forward declare the full list of states. This is indexed by usb_pe_state */
static const struct usb_state pe_states[];

/*
 * We will use DEBUG LABELS if we will be able to print (COMMON RUNTIME)
 * and either CONFIG_USB_PD_DEBUG_LEVEL is not defined (no override) or
 * we are overriding and the level is not DISABLED.
 *
 * If we can't print or the CONFIG_USB_PD_DEBUG_LEVEL is defined to be 0
 * then the DEBUG LABELS will be removed from the build.
 */
#if defined(CONFIG_COMMON_RUNTIME) && (!defined(CONFIG_USB_PD_DEBUG_LEVEL) || \
				       (CONFIG_USB_PD_DEBUG_LEVEL > 0))
#define USB_PD_DEBUG_LABELS
#endif

/* List of human readable state names for console debugging */
__maybe_unused static __const_data const char *const pe_state_names[] = {
/* Super States */
	[PE_PRS_FRS_SHARED] = "SS:PE_PRS_FRS_SHARED",
	[PE_SNK_STARTUP] = "PE_SNK_Startup",
	[PE_SNK_DISCOVERY] = "PE_SNK_Discovery",
	[PE_SNK_WAIT_FOR_CAPABILITIES] = "PE_SNK_Wait_for_Capabilities",
	[PE_SNK_EVALUATE_CAPABILITY] = "PE_SNK_Evaluate_Capability",
	[PE_SNK_SELECT_CAPABILITY] = "PE_SNK_Select_Capability",
	[PE_SNK_READY] = "PE_SNK_Ready",
	[PE_SNK_HARD_RESET] = "PE_SNK_Hard_Reset",
	[PE_SNK_TRANSITION_TO_DEFAULT] = "PE_SNK_Transition_to_default",
	[PE_SNK_GIVE_SINK_CAP] = "PE_SNK_Give_Sink_Cap",
	[PE_SNK_GET_SOURCE_CAP] = "PE_SNK_Get_Source_Cap",
	[PE_SNK_TRANSITION_SINK] = "PE_SNK_Transition_Sink",
	[PE_SEND_SOFT_RESET] = "PE_Send_Soft_Reset",
	[PE_SOFT_RESET] = "PE_Soft_Reset",
	[PE_SEND_NOT_SUPPORTED] = "PE_Send_Not_Supported",
	[PE_SRC_PING] = "PE_SRC_Ping",
	[PE_DRS_EVALUATE_SWAP] = "PE_DRS_Evaluate_Swap",
	[PE_DRS_CHANGE] = "PE_DRS_Change",
	[PE_DRS_SEND_SWAP] = "PE_DRS_Send_Swap",
	[PE_PRS_SRC_SNK_EVALUATE_SWAP] = "PE_PRS_SRC_SNK_Evaluate_Swap",
	[PE_PRS_SRC_SNK_TRANSITION_TO_OFF] = "PE_PRS_SRC_SNK_Transition_To_Off",
	[PE_PRS_SRC_SNK_ASSERT_RD] = "PE_PRS_SRC_SNK_Assert_Rd",
	[PE_PRS_SRC_SNK_WAIT_SOURCE_ON] = "PE_PRS_SRC_SNK_Wait_Source_On",
	[PE_PRS_SRC_SNK_SEND_SWAP] = "PE_PRS_SRC_SNK_Send_Swap",
	[PE_PRS_SNK_SRC_EVALUATE_SWAP] = "PE_PRS_SNK_SRC_Evaluate_Swap",
	[PE_PRS_SNK_SRC_TRANSITION_TO_OFF] = "PE_PRS_SNK_SRC_Transition_To_Off",
	[PE_PRS_SNK_SRC_ASSERT_RP] = "PE_PRS_SNK_SRC_Assert_Rp",
	[PE_PRS_SNK_SRC_SOURCE_ON] = "PE_PRS_SNK_SRC_Source_On",
	[PE_PRS_SNK_SRC_SEND_SWAP] = "PE_PRS_SNK_SRC_Send_Swap",
	[PE_WAIT_FOR_ERROR_RECOVERY] = "PE_Wait_For_Error_Recovery",
	[PE_BIST_TX] = "PE_Bist_TX",
	[PE_DEU_SEND_ENTER_USB] = "PE_DEU_Send_Enter_USB",
	[PE_DR_GET_SINK_CAP] = "PE_DR_Get_Sink_Cap",
	[PE_DR_SNK_GIVE_SOURCE_CAP] = "PE_DR_SNK_Give_Source_Cap",
	[PE_DR_SRC_GET_SOURCE_CAP] = "PE_DR_SRC_Get_Source_Cap",

/* PD3.0 only states below here*/
	[PE_FRS_SNK_SRC_START_AMS] = "PE_FRS_SNK_SRC_Start_Ams",
	[PE_GET_REVISION] = "PE_Get_Revision",
	[PE_SRC_CHUNK_RECEIVED] = "PE_SRC_Chunk_Received",
	[PE_SNK_CHUNK_RECEIVED] = "PE_SNK_Chunk_Received",
	[PE_SNK_SEND_EPR_MODE_ENTRY] = "PE_SNK_Send_EPR_Mode_Entry",
	[PE_SNK_EPR_MODE_ENTRY_WAIT_FOR_RESPONSE] =
		"PE_SNK_EPR_Mode_Entry_Wait_For_Response",
	[PE_SNK_EPR_KEEP_ALIVE] = "PE_SNK_EPR_Keep_Alive",
	[PE_SNK_SEND_EPR_MODE_EXIT] = "PE_SNK_Send_EPR_Mode_Exit",
	[PE_SNK_EPR_MODE_EXIT_RECEIVED] = "PE_SNK_EPR_Mode_Exit_Received",
};

GEN_NOT_SUPPORTED(PE_VCS_EVALUATE_SWAP);
#define PE_VCS_EVALUATE_SWAP PE_VCS_EVALUATE_SWAP_NOT_SUPPORTED
GEN_NOT_SUPPORTED(PE_VCS_SEND_SWAP);
#define PE_VCS_SEND_SWAP PE_VCS_SEND_SWAP_NOT_SUPPORTED
GEN_NOT_SUPPORTED(PE_VCS_WAIT_FOR_VCONN_SWAP);
#define PE_VCS_WAIT_FOR_VCONN_SWAP PE_VCS_WAIT_FOR_VCONN_SWAP_NOT_SUPPORTED
GEN_NOT_SUPPORTED(PE_VCS_TURN_ON_VCONN_SWAP);
#define PE_VCS_TURN_ON_VCONN_SWAP PE_VCS_TURN_ON_VCONN_SWAP_NOT_SUPPORTED
GEN_NOT_SUPPORTED(PE_VCS_TURN_OFF_VCONN_SWAP);
#define PE_VCS_TURN_OFF_VCONN_SWAP PE_VCS_TURN_OFF_VCONN_SWAP_NOT_SUPPORTED
GEN_NOT_SUPPORTED(PE_VCS_SEND_PS_RDY_SWAP);
#define PE_VCS_SEND_PS_RDY_SWAP PE_VCS_SEND_PS_RDY_SWAP_NOT_SUPPORTED


GEN_NOT_SUPPORTED(PE_VCS_FORCE_VCONN);
#define PE_VCS_FORCE_VCONN PE_VCS_FORCE_VCONN_NOT_SUPPORTED

GEN_NOT_SUPPORTED(PE_GIVE_BATTERY_CAP);
#define PE_GIVE_BATTERY_CAP PE_GIVE_BATTERY_CAP_NOT_SUPPORTED
GEN_NOT_SUPPORTED(PE_GIVE_BATTERY_STATUS);
#define PE_GIVE_BATTERY_STATUS PE_GIVE_BATTERY_STATUS_NOT_SUPPORTED
GEN_NOT_SUPPORTED(PE_GIVE_STATUS);
#define PE_GIVE_STATUS PE_GIVE_STATUS_NOT_SUPPORTED
GEN_NOT_SUPPORTED(PE_SEND_ALERT);
#define PE_SEND_ALERT PE_SEND_ALERT_NOT_SUPPORTED


GEN_NOT_SUPPORTED(PE_UDR_SEND_DATA_RESET);
#define PE_UDR_SEND_DATA_RESET PE_UDR_SEND_DATA_RESET_NOT_SUPPORTED
GEN_NOT_SUPPORTED(PE_UDR_DATA_RESET_RECEIVED);
#define PE_UDR_DATA_RESET_RECEIVED PE_UDR_DATA_RESET_RECEIVED_NOT_SUPPORTED
GEN_NOT_SUPPORTED(PE_UDR_TURN_OFF_VCONN);
#define PE_UDR_TURN_OFF_VCONN PE_UDR_TURN_OFF_VCONN_NOT_SUPPORTED
GEN_NOT_SUPPORTED(PE_UDR_SEND_PS_RDY);
#define PE_UDR_SEND_PS_RDY PE_UDR_SEND_PS_RDY_NOT_SUPPORTED
GEN_NOT_SUPPORTED(PE_UDR_WAIT_FOR_DATA_RESET_COMPLETE);
#define PE_UDR_WAIT_FOR_DATA_RESET_COMPLETE \
	PE_UDR_WAIT_FOR_DATA_RESET_COMPLETE_NOT_SUPPORTED
GEN_NOT_SUPPORTED(PE_DDR_SEND_DATA_RESET);
#define PE_DDR_SEND_DATA_RESET PE_DDR_SEND_DATA_RESET_NOT_SUPPORTED
GEN_NOT_SUPPORTED(PE_DDR_DATA_RESET_RECEIVED);
#define PE_DDR_DATA_RESET_RECEIVED PE_DDR_DATA_RESET_RECEIVED_NOT_SUPPORTED
GEN_NOT_SUPPORTED(PE_DDR_WAIT_FOR_VCONN_OFF);
#define PE_DDR_WAIT_FOR_VCONN_OFF PE_DDR_WAIT_FOR_VCONN_OFF_NOT_SUPPORTED
GEN_NOT_SUPPORTED(PE_DDR_PERFORM_DATA_RESET);
#define PE_DDR_PERFORM_DATA_RESET PE_DDR_PERFORM_DATA_RESET_NOT_SUPPORTED

static enum sm_local_state local_state[CONFIG_USB_PD_PORT_MAX_COUNT];

/*
 * Common message send checking
 *
 * PE_MSG_SEND_PENDING:   A message has been requested to be sent.  It has
 *                        not been GoodCRCed or Discarded.
 * PE_MSG_SEND_COMPLETED: The message that was requested has been sent.
 *                        This will only be returned one time and any other
 *                        request for message send status will just return
 *                        PE_MSG_SENT. This message actually includes both
 *                        The COMPLETED and the SENT bit for easier checking.
 *                        NOTE: PE_MSG_SEND_COMPLETED will only be returned
 *                        a single time, directly after TX_COMPLETE.
 * PE_MSG_SENT:           The message that was requested to be sent has
 *                        successfully been transferred to the partner.
 * PE_MSG_DISCARDED:      The message that was requested to be sent was
 *                        discarded.  The partner did not receive it.
 *                        NOTE: PE_MSG_DISCARDED will only be returned
 *                        one time and it is up to the caller to process
 *                        what ever is needed to handle the Discard.
 * PE_MSG_DPM_DISCARDED:  The message that was requested to be sent was
 *                        discarded and an active DRP_REQUEST was active.
 *                        The DRP_REQUEST that was current will be moved
 *                        back to the drp_requests so it can be performed
 *                        later if needed.
 *                        NOTE: PE_MSG_DPM_DISCARDED will only be returned
 *                        one time and it is up to the caller to process
 *                        what ever is needed to handle the Discard.
 */
enum pe_msg_check {
	PE_MSG_SEND_PENDING = BIT(0),
	PE_MSG_SENT = BIT(1),
	PE_MSG_DISCARDED = BIT(2),

	PE_MSG_SEND_COMPLETED = BIT(3) | PE_MSG_SENT,
	PE_MSG_DPM_DISCARDED = BIT(4) | PE_MSG_DISCARDED,
};
static void pe_sender_response_msg_entry(const int port);
static enum pe_msg_check pe_sender_response_msg_run(const int port);
static void pe_sender_response_msg_exit(const int port);

/* Debug log level - higher number == more log */
#ifdef CONFIG_USB_PD_DEBUG_LEVEL
static const enum debug_level pe_debug_level = CONFIG_USB_PD_DEBUG_LEVEL;
#elif defined(CONFIG_USB_PD_INITIAL_DEBUG_LEVEL)
static enum debug_level pe_debug_level = CONFIG_USB_PD_INITIAL_DEBUG_LEVEL;
#else
static enum debug_level pe_debug_level = DEBUG_LEVEL_1;
#endif

/*
 * Policy Engine State Machine Object
 */
static struct policy_engine {
	/* state machine context */
	struct sm_ctx ctx;
	/* current port power role (SOURCE or SINK) */
	enum pd_power_role power_role;
	/* current port data role (DFP or UFP) */
	enum pd_data_role data_role;
	/* state machine flags */
	ATOMIC_DEFINE(flags_a, PE_FLAGS_COUNT);
	/* Device Policy Manager Request */
	atomic_t dpm_request;
	uint32_t dpm_curr_request;
	/* last requested voltage PDO index */
	int requested_idx;

	/*
	 * Port events - PD_STATUS_EVENT_* values
	 * Set from PD task but may be cleared by host command
	 */
	atomic_t events;

	/*
	 * Desired result of a requested VCONN Swap. Only meaningful if
	 * DPM_REQUEST_VCONN_SWAP is active.
	 */
	enum pd_vconn_role requested_vconn_role;

	/* port address where soft resets are sent */
	enum tcpci_msg_type soft_reset_sop;

	/* Current limit / voltage based on the last request message */
	uint32_t curr_limit;
	uint32_t supply_voltage;

	/* PD_VDO_INVALID is used when there is an invalid VDO */
	int32_t ama_vdo;
	int32_t vpd_vdo;
	/* Alternate mode discovery results */
	struct pd_discovery discovery[DISCOVERY_TYPE_COUNT];

	/* Partner type to send */
	enum tcpci_msg_type tx_type;

	/* VDM - used to send information to shared VDM Request state */
	uint32_t vdm_cnt;
	uint32_t vdm_data[VDO_HDR_SIZE + VDO_MAX_SIZE];
	uint8_t vdm_ack_min_data_objects;

	/* ADO - Used to store information about alert messages */
	uint32_t ado;
	mutex_t ado_lock;

	/*
	 * Flag to indicate that the timeout of the current VDM request should
	 * be extended
	 */
	bool vdm_request_extend_timeout;

	/* Counters */

	/*
	 * This counter is used to retry the Hard Reset whenever there is no
	 * response from the remote device.
	 */
	uint32_t hard_reset_counter;

	/*
	 * This counter is used to count the number of Source_Capabilities
	 * Messages which have been sent by a Source at power up or after a
	 * Hard Reset.
	 */
	uint32_t caps_counter;

	/*
	 * This counter maintains a count of Discover Identity Messages sent
	 * to a cable.  If no GoodCRC messages are received after
	 * nDiscoverIdentityCount, the port shall not send any further
	 * SOP'/SOP'' messages.
	 */
	uint32_t discover_identity_counter;
	/*
	 * For PD2.0, we need to be a DFP before sending a discovery identity
	 * message to our port partner. This counter keeps track of how
	 * many attempts to DR SWAP from UFP to DFP.
	 */
	uint32_t dr_swap_attempt_counter;

	/*
	 * This counter tracks how many PR Swap messages are sent when the
	 * partner responds with a Wait message. Only used during SRC to SNK
	 * PR swaps
	 */
	uint8_t src_snk_pr_swap_counter;

	/*
	 * This counter maintains a count of VCONN swap requests. If VCONN swap
	 * isn't successful after N_VCONN_SWAP_COUNT, the port calls
	 * dpm_vdm_naked().
	 */
	uint8_t vconn_swap_counter;

	/* Last received source cap */
	uint32_t src_caps[PDO_MAX_OBJECTS];
	int src_cap_cnt; /* -1 on error retrieving source caps */

	/* Last received sink cap */
	uint32_t snk_caps[PDO_MAX_OBJECTS];
	int snk_cap_cnt;

	/* Last received Revision Message Data Object (RMDO) from the partner */
	struct rmdo partner_rmdo;
} pe[CONFIG_USB_PD_PORT_MAX_COUNT];

test_export_static enum usb_pe_state get_state_pe(const int port);
test_export_static void set_state_pe(const int port,
				     const enum usb_pe_state new_state);
static void pe_set_dpm_curr_request(const int port, const int request);
/*
 * The spec. revision is used to index into this array.
 *  PD 1.0 (VDO 1.0) - return SVDM_VER_1_0
 *  PD 2.0 (VDO 1.0) - return SVDM_VER_1_0
 *  PD 3.0 (VDO 2.0) - return SVDM_VER_1_0
 */
static const uint8_t vdo_ver[] = {
	[PD_REV10] = SVDM_VER_1_0,
	[PD_REV20] = SVDM_VER_1_0,
	[PD_REV30] = SVDM_VER_2_0,
};

int pd_get_rev(int port, enum tcpci_msg_type type)
{
	return prl_get_rev(port, type);
}

int pd_get_vdo_ver(int port, enum tcpci_msg_type type)
{
	enum pd_rev_type rev = prl_get_rev(port, type);

	if (rev < PD_REV30)
		return vdo_ver[rev];
	else
		return SVDM_VER_2_0;
}

static void pe_set_ready_state(int port)
{
	if (pe[port].power_role == PD_ROLE_SOURCE)
		set_state_pe(port, PE_SRC_READY);
	else
		set_state_pe(port, PE_SNK_READY);
}

static void pe_set_hard_reset(int port)
{
	if (pe[port].power_role == PD_ROLE_SOURCE)
		set_state_pe(port, PE_SRC_HARD_RESET);
	else
		set_state_pe(port, PE_SNK_HARD_RESET);
}

static inline void send_data_msg(int port, enum tcpci_msg_type type,
				 enum pd_data_msg_type msg)
{
	/* Clear any previous TX status before sending a new message */
	PE_CLR_FLAG(port, PE_FLAGS_TX_COMPLETE);
	prl_send_data_msg(port, type, msg);
}

static __maybe_unused inline void
send_ext_data_msg(int port, enum tcpci_msg_type type, enum pd_ext_msg_type msg)
{
	/* Clear any previous TX status before sending a new message */
	PE_CLR_FLAG(port, PE_FLAGS_TX_COMPLETE);
	prl_send_ext_data_msg(port, type, msg);
}

static inline void send_ctrl_msg(int port, enum tcpci_msg_type type,
				 enum pd_ctrl_msg_type msg)
{
	/* Clear any previous TX status before sending a new message */
	PE_CLR_FLAG(port, PE_FLAGS_TX_COMPLETE);
	prl_send_ctrl_msg(port, type, msg);
}

/* Set both the SOP' and SOP'' revisions to the given value */
static void set_cable_rev(int port, enum pd_rev_type rev)
{
	prl_set_rev(port, TCPCI_MSG_SOP_PRIME, rev);
	prl_set_rev(port, TCPCI_MSG_SOP_PRIME_PRIME, rev);
}

/* Initialize the cable revision based only on the partner SOP revision */
static void init_cable_rev(int port)
{
	/*
	 * If port partner runs PD 2.0, cable communication must
	 * also be PD 2.0
	 */
	if (prl_get_rev(port, TCPCI_MSG_SOP) == PD_REV20) {
		/*
		 * If the cable supports PD 3.0, but the port partner supports
		 * PD 2.0, redo the cable discover with PD 2.0
		 */
		if (prl_get_rev(port, TCPCI_MSG_SOP_PRIME) == PD_REV30 &&
		    pd_get_identity_discovery(port, TCPCI_MSG_SOP_PRIME) ==
			    PD_DISC_COMPLETE) {
			pd_set_identity_discovery(port, TCPCI_MSG_SOP_PRIME,
						  PD_DISC_NEEDED);
		}
		set_cable_rev(port, PD_REV20);
	}
}

/* Compile-time insurance to ensure this code does not call into prl directly */
#define prl_send_data_msg DO_NOT_USE
#define prl_send_ext_data_msg DO_NOT_USE
#define prl_send_ctrl_msg DO_NOT_USE

static void pe_init(int port)
{
	memset(&pe[port].flags_a, 0, sizeof(pe[port].flags_a));
	pe[port].dpm_request = 0;
	pe[port].dpm_curr_request = 0;
	pd_timer_disable_range(port, PE_TIMER_RANGE);
	pe[port].data_role = pd_get_data_role(port);
	pe[port].tx_type = TCPCI_MSG_INVALID;
	pe[port].events = 0;

	tc_pd_connection(port, 0);

	if (pd_get_power_role(port) == PD_ROLE_SOURCE)
		set_state_pe(port, PE_SRC_STARTUP);
	else
		set_state_pe(port, PE_SNK_STARTUP);
}

int pe_is_running(int port)
{
	return local_state[port] == SM_RUN;
}

bool pe_in_frs_mode(int port)
{
	return PE_CHK_FLAG(port, PE_FLAGS_FAST_ROLE_SWAP_PATH);
}

bool pe_in_local_ams(int port)
{
	return !!PE_CHK_FLAG(port, PE_FLAGS_LOCALLY_INITIATED_AMS);
}

void pe_set_debug_level(enum debug_level debug_level)
{
#ifndef CONFIG_USB_PD_DEBUG_LEVEL
	pe_debug_level = debug_level;
#endif
}

void pe_run(int port, int evt, int en)
{
	switch (local_state[port]) {
	case SM_PAUSED:
		if (!en)
			break;
		__fallthrough;
	case SM_INIT:
		pe_init(port);
		local_state[port] = SM_RUN;
		__fallthrough;
	case SM_RUN:
		if (!en) {
			local_state[port] = SM_PAUSED;
			/*
			 * While we are paused, exit all states and wait until
			 * initialized again.
			 */
			set_state(port, &pe[port].ctx, NULL);
			break;
		}

		/*
		 * 8.3.3.3.8 PE_SNK_Hard_Reset State
		 * The Policy Engine Shall transition to the PE_SNK_Hard_Reset
		 * state from any state when:
		 * - Hard Reset request from Device Policy Manager
		 *
		 * USB PD specification clearly states that we should go to
		 * PE_SNK_Hard_Reset from ANY state (including states in which
		 * port is source) when DPM requests that. This can lead to
		 * execute Hard Reset path for sink when actually our power
		 * role is source. In our implementation we will choose Hard
		 * Reset path depending on current power role.
		 */
		if (PE_CHK_DPM_REQUEST(port, DPM_REQUEST_HARD_RESET_SEND)) {
			/*
			 * If a hard reset condition came up during FRS, we must
			 * go into ErrorRecovery.  Performing a hard reset could
			 * leave us assuming our own FRS Vbus is coming from the
			 * partner and leave the port stuck as Attached.SNK
			 */
			if (pe_in_frs_mode(port)) {
				PE_CLR_DPM_REQUEST(port,
						   DPM_REQUEST_HARD_RESET_SEND);
				set_state_pe(port, PE_WAIT_FOR_ERROR_RECOVERY);
			} else {
				pe_set_dpm_curr_request(
					port, DPM_REQUEST_HARD_RESET_SEND);
				pe_set_hard_reset(port);
			}
		}

		/*
		 * Check for Fast Role Swap signal
		 * This is not a typical pattern for adding state changes.
		 * I added this here because FRS SIGNALED can happen at any
		 * state once we are listening for the signal and we want to
		 * make sure to handle it immediately.
		 */
		if (1/*IS_ENABLED(CONFIG_USB_PD_REV30)*/ &&
		    PE_CHK_FLAG(port, PE_FLAGS_FAST_ROLE_SWAP_SIGNALED)) {
			PE_CLR_FLAG(port, PE_FLAGS_FAST_ROLE_SWAP_SIGNALED);
			set_state_pe(port, PE_FRS_SNK_SRC_START_AMS);
		}

		/* Run state machine */
		run_state(port, &pe[port].ctx);
		break;
	}
}

int pe_is_explicit_contract(int port)
{
	return PE_CHK_FLAG(port, PE_FLAGS_EXPLICIT_CONTRACT);
}

void pe_message_received(int port)
{
	/* This should only be called from the PD task */
	assert(port == TASK_ID_TO_PD_PORT(task_get_current()));

	PE_SET_FLAG(port, PE_FLAGS_MSG_RECEIVED);
	pd_loop_wake(port);
}

void pe_hard_reset_sent(int port)
{
	/* This should only be called from the PD task */
	assert(port == TASK_ID_TO_PD_PORT(task_get_current()));

	PE_CLR_FLAG(port, PE_FLAGS_HARD_RESET_PENDING);
}

void pe_got_hard_reset(int port)
{
	/* This should only be called from the PD task */
	assert(port == TASK_ID_TO_PD_PORT(task_get_current()));

	/*
	 * If we're in the middle of an FRS, any error should cause us to follow
	 * the ErrorRecovery path
	 */
	if (pe_in_frs_mode(port)) {
		set_state_pe(port, PE_WAIT_FOR_ERROR_RECOVERY);
		return;
	}

	/*
	 * Transition from any state to the PE_SRC_Hard_Reset_Received or
	 *  PE_SNK_Transition_to_default state when:
	 *  1) Hard Reset Signaling is detected.
	 */
	pe[port].power_role = pd_get_power_role(port);

	/* Exit BIST Test mode, in case the TCPC entered it. */
	tcpc_set_bist_test_mode(port, false);

	if (pe[port].power_role == PD_ROLE_SOURCE)
		set_state_pe(port, PE_SRC_HARD_RESET_RECEIVED);
	else
		set_state_pe(port, PE_SNK_TRANSITION_TO_DEFAULT);
}

/*
 * pd_got_frs_signal
 *
 * Called by the handler that detects the FRS signal in order to
 * switch PE states to complete the FRS that the hardware has
 * started.
 *
 * If the PE is not running, generate an error recovery to turn off
 * Vbus and get the port back into a known state.
 */
test_mockable void pd_got_frs_signal(int port)
{
	if (pe_is_running(port))
		PE_SET_FLAG(port, PE_FLAGS_FAST_ROLE_SWAP_SIGNALED);
	else
		pd_set_error_recovery(port);

	pd_loop_wake(port);
}

/*
 * PE_Set_FRS_Enable
 *
 * This function should be called every time an explicit contract
 * is disabled, to disable FRS.
 *
 * Enabling an explicit contract is not enough to enable FRS, it
 * also requires a Sink Capability power requirement from a Source
 * that supports FRS so we can determine if this is something we
 * can handle.
 */
static void pe_set_frs_enable(int port, int enable)
{
	int current = PE_CHK_FLAG(port, PE_FLAGS_FAST_ROLE_SWAP_ENABLED);

	/* This should only be called from the PD task */
	if (!IS_ENABLED(TEST_BUILD))
		assert(port == TASK_ID_TO_PD_PORT(task_get_current()));

	if (!0/*IS_ENABLED(CONFIG_USB_PD_FRS)*/ || !1/*IS_ENABLED(CONFIG_USB_PD_REV30)*/)
		return;

	/* Request an FRS change, only if the state has changed */
	if (!!current == !!enable)
		return;

	pd_set_frs_enable(port, enable);
	if (enable) {
		int curr_limit = *pd_get_snk_caps(port) &
				 PDO_FIXED_FRS_CURR_MASK;

		typec_select_src_current_limit_rp(
			port, curr_limit == PDO_FIXED_FRS_CURR_3A0_AT_5V ?
				      TYPEC_RP_3A0 :
				      TYPEC_RP_1A5);
		PE_SET_FLAG(port, PE_FLAGS_FAST_ROLE_SWAP_ENABLED);
	} else {
		PE_CLR_FLAG(port, PE_FLAGS_FAST_ROLE_SWAP_ENABLED);
	}
}

void pe_set_explicit_contract(int port)
{
	PE_SET_FLAG(port, PE_FLAGS_EXPLICIT_CONTRACT);

	/* Set Rp for collision avoidance */
	if (1/*IS_ENABLED(CONFIG_USB_PD_REV30)*/)
		typec_update_cc(port);
}

/*
 * Invalidate the explicit contract without disabling FRS.
 *
 * @param port USB-C port number
 */
static void pe_invalidate_explicit_contract_frs_untouched(int port)
{
	PE_CLR_FLAG(port, PE_FLAGS_EXPLICIT_CONTRACT);

	/* Set Rp for current limit if still attached */
	if (1/*IS_ENABLED(CONFIG_USB_PD_REV30)*/ && pd_is_connected(port))
		typec_update_cc(port);
}

void pe_invalidate_explicit_contract(int port)
{
	/* disable FRS and then invalidate the explicit contract */
	pe_set_frs_enable(port, 0);

	pe_invalidate_explicit_contract_frs_untouched(port);
}

void pd_notify_event(int port, uint32_t event_mask)
{
	atomic_or(&pe[port].events, event_mask);

	/* Notify the host that new events are available to read */
	pd_send_host_event(PD_EVENT_TYPEC);
}

void pd_clear_events(int port, uint32_t clear_mask)
{
	atomic_clear_bits(&pe[port].events, clear_mask);
}

uint32_t pd_get_events(int port)
{
	return pe[port].events;
}

void pe_set_snk_caps(int port, int cnt, uint32_t *snk_caps)
{
	pe[port].snk_cap_cnt = cnt;

	memcpy(pe[port].snk_caps, snk_caps, sizeof(uint32_t) * cnt);
}

const uint32_t *const pd_get_snk_caps(int port)
{
	return pe[port].snk_caps;
}

uint8_t pd_get_snk_cap_cnt(int port)
{
	return pe[port].snk_cap_cnt;
}

uint32_t pd_get_requested_voltage(int port)
{
	return pe[port].supply_voltage;
}

uint32_t pd_get_requested_current(int port)
{
	return pe[port].curr_limit;
}

/**
 * Return true if we're in an SPR contract. Note that before exiting EPR mode,
 * port partners are required first to drop to SPR. Thus, we can be still in
 * EPR mode (with a SPR contract).
 */
static int pe_in_spr_contract(int port)
{
	return pd_get_requested_voltage(port) <= PD_MAX_SPR_VOLTAGE;
}

/*
 * Determine if this port may send the given VDM type
 *
 * For PD 2.0, "Only the DFP Shall be an Initrator of Structured VDMs except for
 * the Attention Command that Shall only be initiated by the UFP"
 *
 * For PD 3.0, "Either port May be an Initiator of Structured VDMs except for
 * the Enter Mode and Exit Mode Commands which shall only be initiated by the
 * DFP" (6.4.4.2 Structured VDM)
 *
 * In both revisions, VDMs may only be initiated while in an explicit contract,
 * with the only exception being for cable plug discovery.
 */
static bool pe_can_send_sop_vdm(int port, int vdm_cmd)
{
	if (PE_CHK_FLAG(port, PE_FLAGS_EXPLICIT_CONTRACT)) {
		if (prl_get_rev(port, TCPCI_MSG_SOP) == PD_REV20) {
			if (pe[port].data_role == PD_ROLE_UFP &&
			    vdm_cmd != CMD_ATTENTION) {
				return false;
			}
		} else {
			if (pe[port].data_role == PD_ROLE_UFP &&
			    (vdm_cmd == CMD_ENTER_MODE ||
			     vdm_cmd == CMD_EXIT_MODE)) {
				return false;
			}
		}
		return true;
	}

	return false;
}

static const uint32_t pd_get_fixed_pdo(int port)
{
	return pe[port].src_caps[0];
}

bool pe_snk_in_epr_mode(int port)
{
	return PE_CHK_FLAG(port, PE_FLAGS_IN_EPR);
}

void pe_snk_epr_explicit_exit(int port)
{
	PE_SET_FLAG(port, PE_FLAGS_EPR_EXPLICIT_EXIT);
}

bool pe_snk_can_enter_epr_mode(int port)
{
	/*
	 * 6.4.10.1 of USB PD R3.1 V1.6
	 *
	 * 1. A Sink Shall Not be Connected to the Source through a CT-VPD.
	 * 2. The Source and Sink Shall be in an SPR Explicit Contract.
	 * 3. The EPR Mode capable bit Shall have been set in the 5V fixed PDO
	 *    in the last Source_Capabilities Message the Sink received.
	 * 4. The EPR Mode capable bit Shall have been set in the RDO in the
	 *    last Request Message the Source received.
	 */
	if (is_vpd_ct_supported(port)) {
		return false;
	}

	if (!pe_is_explicit_contract(port))
		return false;

	if (!(pd_get_fixed_pdo(port) & PDO_FIXED_EPR_MODE_CAPABLE))
		return false;

	return true;
}

static void pe_send_soft_reset(const int port, enum tcpci_msg_type type)
{
	pe[port].soft_reset_sop = type;
	set_state_pe(port, PE_SEND_SOFT_RESET);
}

void pe_report_discard(int port)
{
	/*
	 * Clear local AMS indicator as our AMS message was discarded, and flag
	 * the discard for the PE
	 */
	PE_CLR_FLAG(port, PE_FLAGS_LOCALLY_INITIATED_AMS);
	PE_SET_FLAG(port, PE_FLAGS_MSG_DISCARDED);

	/* TODO(b/157228506): Ensure all states are checking discard */
}

/*
 * Utility function to check for an outgoing message discard during states which
 * send a message as a part of an AMS and wait for the transmit to complete.
 * Note these states should not be power transitioning.
 *
 * In these states, discard due to an incoming message is a protocol error.
 */
static bool pe_check_outgoing_discard(int port)
{
	/*
	 * On outgoing discard, soft reset with SOP* of incoming message
	 *
	 * See Table 6-65 Response to an incoming Message (except VDM) in PD 3.0
	 * Version 2.0 Specification.
	 */
	if (PE_CHK_FLAG(port, PE_FLAGS_MSG_DISCARDED) &&
	    PE_CHK_FLAG(port, PE_FLAGS_MSG_RECEIVED)) {
		enum tcpci_msg_type sop =
			PD_HEADER_GET_SOP(rx_emsg[port].header);

		PE_CLR_FLAG(port, PE_FLAGS_MSG_DISCARDED);
		PE_CLR_FLAG(port, PE_FLAGS_MSG_RECEIVED);

		pe_send_soft_reset(port, sop);
		return true;
	}

	return false;
}

void pe_report_error(int port, enum pe_error e, enum tcpci_msg_type type)
{
	/* This should only be called from the PD task */
	assert(port == TASK_ID_TO_PD_PORT(task_get_current()));

	/*
	 * If there is a timeout error while waiting for a chunk of a chunked
	 * message, there is no requirement to trigger a soft reset.
	 */
	if (e == ERR_RCH_CHUNK_WAIT_TIMEOUT)
		return;

	/*
	 * Generate Hard Reset if Protocol Error occurred
	 * while in PE_Send_Soft_Reset state.
	 */
	if (get_state_pe(port) == PE_SEND_SOFT_RESET) {
		pe_set_hard_reset(port);
		return;
	}

	/*
	 * The following states require custom handling of protocol errors,
	 * because they either need special handling of the no GoodCRC case
	 * (cable identity request, send capabilities), occur before explicit
	 * contract (discovery), or happen during a power transition.
	 *
	 * TODO(b/150774779): TCPMv2: Improve pe_error documentation
	 */
	if ((get_state_pe(port) == PE_SRC_SEND_CAPABILITIES ||
	     get_state_pe(port) == PE_SRC_TRANSITION_SUPPLY ||
	     get_state_pe(port) == PE_PRS_SNK_SRC_EVALUATE_SWAP ||
	     get_state_pe(port) == PE_PRS_SNK_SRC_SOURCE_ON ||
	     get_state_pe(port) == PE_PRS_SRC_SNK_WAIT_SOURCE_ON ||
	     get_state_pe(port) == PE_SRC_DISABLED ||
	     get_state_pe(port) == PE_SRC_DISCOVERY ||
	     get_state_pe(port) == PE_VCS_CBL_SEND_SOFT_RESET ||
	     get_state_pe(port) == PE_VDM_IDENTITY_REQUEST_CBL) ||
	    (0/*IS_ENABLED(CONFIG_USB_PD_DATA_RESET_MSG)*/ &&
	     (get_state_pe(port) == PE_UDR_SEND_DATA_RESET ||
	      get_state_pe(port) == PE_UDR_DATA_RESET_RECEIVED ||
	      get_state_pe(port) == PE_UDR_TURN_OFF_VCONN ||
	      get_state_pe(port) == PE_UDR_SEND_PS_RDY ||
	      get_state_pe(port) == PE_UDR_WAIT_FOR_DATA_RESET_COMPLETE ||
	      get_state_pe(port) == PE_DDR_SEND_DATA_RESET ||
	      get_state_pe(port) == PE_DDR_DATA_RESET_RECEIVED ||
	      get_state_pe(port) == PE_DDR_WAIT_FOR_VCONN_OFF ||
	      get_state_pe(port) == PE_DDR_PERFORM_DATA_RESET)) ||
	    (pe_in_frs_mode(port) &&
	     get_state_pe(port) == PE_PRS_SNK_SRC_SEND_SWAP)) {
		PE_SET_FLAG(port, PE_FLAGS_PROTOCOL_ERROR);
		pd_loop_wake(port);
		return;
	}

	/*
	 * See section 8.3.3.4.1.1 PE_SRC_Send_Soft_Reset State:
	 *
	 * The PE_Send_Soft_Reset state shall be entered from
	 * any state when
	 * * A Protocol Error is detected by Protocol Layer during a
	 *   Non-Interruptible AMS or
	 * * A message has not been sent after retries or
	 * * When not in an explicit contract and
	 *   * Protocol Errors occurred on SOP during an Interruptible AMS or
	 *   * Protocol Errors occurred on SOP during any AMS where the first
	 *     Message in the sequence has not yet been sent i.e. an unexpected
	 *     Message is received instead of the expected GoodCRC Message
	 *     response.
	 */
	/* All error types besides transmit errors are Protocol Errors. */
	if ((e != ERR_TCH_XMIT &&
	     !PE_CHK_FLAG(port, PE_FLAGS_INTERRUPTIBLE_AMS)) ||
	    e == ERR_TCH_XMIT ||
	    (!PE_CHK_FLAG(port, PE_FLAGS_EXPLICIT_CONTRACT) &&
	     type == TCPCI_MSG_SOP)) {
		pe_send_soft_reset(port, type);
	}
	/*
	 * Transition to PE_Snk_Ready or PE_Src_Ready by a Protocol
	 * Error during an Interruptible AMS.
	 */
	else {
		pe_set_ready_state(port);
	}
}

void pe_got_soft_reset(int port)
{
	/* This should only be called from the PD task */
	assert(port == TASK_ID_TO_PD_PORT(task_get_current()));

	/*
	 * The PE_SRC_Soft_Reset state Shall be entered from any state when a
	 * Soft_Reset Message is received from the Protocol Layer.
	 *
	 * However, if we're in the middle of an FRS sequence, we need to go to
	 * ErrorRecovery instead.
	 */
	set_state_pe(port, pe_in_frs_mode(port) ? PE_WAIT_FOR_ERROR_RECOVERY :
						  PE_SOFT_RESET);
}

__overridable bool pd_can_charge_from_device(int port, const int pdo_cnt,
					     const uint32_t *pdos)
{
	/*
	 * Don't attempt to charge from a device we have no SrcCaps from. Or, if
	 * drp_state is FORCE_SOURCE then don't attempt a PRS.
	 */
	if (pdo_cnt == 0 || pd_get_dual_role(port) == PD_DRP_FORCE_SOURCE)
		return false;

	/*
	 * Treat device as a dedicated charger (meaning we should charge
	 * from it) if:
	 *   - it does not support power swap, or
	 *   - it is unconstrained power, or
	 *   - it presents at least 27 W of available power
	 */

	/* Unconstrained Power or NOT Dual Role Power we can charge from */
	if (pdos[0] & PDO_FIXED_UNCONSTRAINED ||
	    (pdos[0] & PDO_FIXED_DUAL_ROLE) == 0)
		return true;

	/* [virtual] allow_list */
	if (0/*IS_ENABLED(CONFIG_CHARGE_MANAGER)*/) {
		uint32_t max_ma, max_mv, max_pdo, max_mw, unused;

		/*
		 * Get max power that the partner offers (not necessarily what
		 * this board will request)
		 */
		pd_find_pdo_index(pdo_cnt, pdos, pd_get_max_voltage(),
				  &max_pdo);
		pd_extract_pdo_power(max_pdo, &max_ma, &max_mv, &unused);
		max_mw = max_ma * max_mv / 1000;

		if (max_mw >= PD_DRP_CHARGE_POWER_MIN)
			return true;
	}
	return false;
}

void pd_resume_check_pr_swap_needed(int port)
{
	/*
	 * Explicit contract, current power role of SNK, the device
	 * indicates it should not power us, and device isn't selected
	 * as the charging port (ex. through the GUI) then trigger a PR_Swap
	 */
	if (pe_is_explicit_contract(port) &&
	    pd_get_power_role(port) == PD_ROLE_SINK &&
	    !pd_can_charge_from_device(port, pd_get_src_cap_cnt(port),
				       pd_get_src_caps(port)) &&
	    (!0/*IS_ENABLED(CONFIG_CHARGE_MANAGER)*/ ||
	     charge_manager_get_active_charge_port() != port))
		pd_dpm_request(port, DPM_REQUEST_PR_SWAP);
}

void pd_dpm_request(int port, enum pd_dpm_request req)
{
	PE_SET_DPM_REQUEST(port, req);
}

void pe_vconn_swap_complete(int port)
{
	/* This should only be called from the PD task */
	assert(port == TASK_ID_TO_PD_PORT(task_get_current()));

	PE_SET_FLAG(port, PE_FLAGS_VCONN_SWAP_COMPLETE);
}

void pe_ps_reset_complete(int port)
{
	/* This should only be called from the PD task */
	assert(port == TASK_ID_TO_PD_PORT(task_get_current()));

	PE_SET_FLAG(port, PE_FLAGS_PS_RESET_COMPLETE);
}

void pe_message_sent(int port)
{
	/* This should only be called from the PD task */
	assert(port == TASK_ID_TO_PD_PORT(task_get_current()));

	PE_SET_FLAG(port, PE_FLAGS_TX_COMPLETE);
	pd_loop_wake(port);
}

void pd_send_vdm(int port, uint32_t vid, int cmd, const uint32_t *data,
		 int count)
{
	/* Copy VDM Header */
	pe[port].vdm_data[0] = VDO(
		vid,
		((vid & USB_SID_PD) == USB_SID_PD) ?
			1 :
			(PD_VDO_CMD(cmd) <= CMD_ATTENTION),
		VDO_SVDM_VERS_MAJOR(pd_get_vdo_ver(port, TCPCI_MSG_SOP)) | cmd);

	/*
	 * Copy VDOs after the VDM Header. Note that the count refers to VDO
	 * count.
	 */
	memcpy((pe[port].vdm_data + 1), data, count * sizeof(uint32_t));

	pe[port].vdm_cnt = count + 1;

	/*
	 * The PE transmit routine assumes that tx_type was set already. Note,
	 * that this function is likely called from outside the PD task.
	 * (b/180465870)
	 */
	pe[port].tx_type = TCPCI_MSG_SOP;
	pd_dpm_request(port, DPM_REQUEST_VDM);

	pd_loop_wake(port);
}

#ifdef TEST_BUILD
/*
 * Allow unit tests to access this function to clear internal state data between
 * runs
 */
void pe_clear_port_data(int port)
#else
static void pe_clear_port_data(int port)
#endif /* TEST_BUILD */
{
	/*
	 * PD 3.0 Section 8.3.3.3.8
	 * Note: The HardResetCounter is reset on a power cycle or Detach.
	 */
	pe[port].hard_reset_counter = 0;

	/* Reset port events */
	pd_clear_events(port, GENMASK(31, 0));

	/* But then set disconnected event */
	pd_notify_event(port, PD_STATUS_EVENT_DISCONNECTED);

	/* Tell Policy Engine to invalidate the explicit contract */
	pe_invalidate_explicit_contract(port);

	/*
	 * Saved Source and Sink Capabilities are no longer valid on disconnect
	 */
	pd_set_src_caps(port, 0, NULL);
	pe_set_snk_caps(port, 0, NULL);

	/*
	 * Saved Revision responses are no longer valid on disconnect
	 */
	pe[port].partner_rmdo.reserved = 0;
	pe[port].partner_rmdo.minor_ver = 0;
	pe[port].partner_rmdo.major_ver = 0;
	pe[port].partner_rmdo.minor_rev = 0;
	pe[port].partner_rmdo.major_rev = 0;

	/* Clear any stored discovery data, but leave modes for alt mode exit */
	pd_dfp_discovery_init(port);

	/* Clear any pending alerts */
	pe_clear_ado(port);

	dpm_remove_sink(port);
	dpm_remove_source(port);
	dpm_init(port);

	/* Exit BIST Test mode, in case the TCPC entered it. */
	tcpc_set_bist_test_mode(port, false);
}

void pe_set_requested_vconn_role(int port, enum pd_vconn_role role)
{
	pe[port].requested_vconn_role = role;
}

int pe_set_ado(int port, uint32_t data)
{
	/* return busy error if unable to set ado */
	int ret = EC_ERROR_BUSY;

	mutex_lock(&pe[port].ado_lock);
	if (pe[port].ado == 0x0) {
		pe[port].ado = data;
		ret = EC_SUCCESS;
	}

	mutex_unlock(&pe[port].ado_lock);
	return ret;
}

void pe_clear_ado(int port)
{
	mutex_lock(&pe[port].ado_lock);
	pe[port].ado = 0x0;
	mutex_unlock(&pe[port].ado_lock);
}

struct rmdo pd_get_partner_rmdo(int port)
{
	return pe[port].partner_rmdo;
}

static void pe_handle_detach(void)
{
	const int port = TASK_ID_TO_PD_PORT(task_get_current());

	pe_clear_port_data(port);
}
DECLARE_HOOK(HOOK_USB_PD_DISCONNECT, pe_handle_detach, HOOK_PRIO_DEFAULT);


/*
 * Private functions
 */
static void pe_set_dpm_curr_request(const int port, const int request)
{
	PE_CLR_DPM_REQUEST(port, request);
	pe[port].dpm_curr_request = request;
}

/* Set the TypeC state machine to a new state. */
test_export_static void set_state_pe(const int port,
				     const enum usb_pe_state new_state)
{
	set_state(port, &pe[port].ctx, &pe_states[new_state]);
}

/* Get the current TypeC state. */
test_export_static enum usb_pe_state get_state_pe(const int port)
{
	return pe[port].ctx.current - &pe_states[0];
}

/*
 * PD 3.x partners should respond to Data_Reset with either Accept or
 * Not_Supported. However, some partners simply do not respond, triggering
 * ErrorRecovery. Try to avoid this by only initiating Data Reset with partners
 * that seem likely to support it.
 */
static bool pe_should_send_data_reset(const int port)
{
	const struct pd_discovery *disc =
		pd_get_am_discovery(port, TCPCI_MSG_SOP);
	const enum idh_ptype ufp_ptype = pd_get_product_type(port);
	const union ufp_vdo_rev30 ufp_vdo = {
		.raw_value = disc->identity_cnt >= VDO_INDEX_PTYPE_UFP1_VDO ?
				     disc->identity.product_t1.raw_value :
				     0
	};

	return prl_get_rev(port, TCPCI_MSG_SOP) >= PD_REV30 &&
	       /*
		* Data Reset was added to the PD spec around the time that the
		* AMA product type/VDO was deprecated and the UFP VDO added.
		* Partners that advertise the AMA product type are thus likely
		* not to support Data Reset (and perhaps more likely than newer
		* products to not respond to it at all).
		*/
	       (ufp_ptype == IDH_PTYPE_HUB || ufp_ptype == IDH_PTYPE_PERIPH) &&
	       ((ufp_vdo.device_capability & VDO_UFP1_CAPABILITY_USB4) ||
		ufp_vdo.alternate_modes);
}

/*
 * Handle common DPM requests to both source and sink.
 *
 * Note: it is assumed the calling state set PE_FLAGS_LOCALLY_INITIATED_AMS
 *
 * Returns true if state was set and calling run state should now return.
 */
static bool common_src_snk_dpm_requests(int port)
{
	if (0/*IS_ENABLED(CONFIG_USBC_VCONN)*/ &&
	    PE_CHK_DPM_REQUEST(port, DPM_REQUEST_VCONN_SWAP)) {
		enum pd_vconn_role request = pe[port].requested_vconn_role;
		enum pd_vconn_role current = pd_get_vconn_state(port) ?
						     PD_ROLE_VCONN_SRC :
						     PD_ROLE_VCONN_OFF;
		if (request == current) {
			PE_CLR_DPM_REQUEST(port, DPM_REQUEST_DATA_RESET);
			return false;
		}
		pe_set_dpm_curr_request(port, DPM_REQUEST_VCONN_SWAP);
		set_state_pe(port, PE_VCS_SEND_SWAP);
		return true;
	} else if (PE_CHK_DPM_REQUEST(port, DPM_REQUEST_BIST_TX)) {
		pe_set_dpm_curr_request(port, DPM_REQUEST_BIST_TX);
		set_state_pe(port, PE_BIST_TX);
		return true;
	} else if (PE_CHK_DPM_REQUEST(port, DPM_REQUEST_SNK_STARTUP)) {
		pe_set_dpm_curr_request(port, DPM_REQUEST_SNK_STARTUP);
		set_state_pe(port, PE_SNK_STARTUP);
		return true;
	} else if (PE_CHK_DPM_REQUEST(port, DPM_REQUEST_SRC_STARTUP)) {
		pe_set_dpm_curr_request(port, DPM_REQUEST_SRC_STARTUP);
		set_state_pe(port, PE_SRC_STARTUP);
		return true;
	} else if (PE_CHK_DPM_REQUEST(port, DPM_REQUEST_SOFT_RESET_SEND)) {
		pe_set_dpm_curr_request(port, DPM_REQUEST_SOFT_RESET_SEND);
		/* Currently only support sending soft reset to SOP */
		pe_send_soft_reset(port, TCPCI_MSG_SOP);
		return true;
	} else if (PE_CHK_DPM_REQUEST(port, DPM_REQUEST_PORT_DISCOVERY)) {
		pe_set_dpm_curr_request(port, DPM_REQUEST_PORT_DISCOVERY);
		if (!PE_CHK_FLAG(port, PE_FLAGS_MODAL_OPERATION)) {
			/*
			 * Clear counters and reset timer to trigger a
			 * port discovery, and also clear any pending VDM send
			 * requests.
			 */
			pd_dfp_discovery_init(port);
			/*
			 * TODO(b/189353401): Do not reinitialize modes when no
			 * longer required.
			 */
			pd_dfp_mode_init(port);
			pe[port].dr_swap_attempt_counter = 0;
			pe[port].discover_identity_counter = 0;
			pd_timer_enable(port, PE_TIMER_DISCOVER_IDENTITY,
					PD_T_DISCOVER_IDENTITY);
			PE_CLR_DPM_REQUEST(port, DPM_REQUEST_VDM);
		}
		return true;
	} else if (PE_CHK_DPM_REQUEST(port, DPM_REQUEST_VDM)) {
		pe_set_dpm_curr_request(port, DPM_REQUEST_VDM);
		/* Send previously set up SVDM. */
		set_state_pe(port, PE_VDM_REQUEST_DPM);
		return true;
	} else if (PE_CHK_DPM_REQUEST(port, DPM_REQUEST_ENTER_USB)) {
		pe_set_dpm_curr_request(port, DPM_REQUEST_ENTER_USB);
		set_state_pe(port, PE_DEU_SEND_ENTER_USB);
		return true;
	} else if (PE_CHK_DPM_REQUEST(port, DPM_REQUEST_EXIT_MODES)) {
		pe_set_dpm_curr_request(port, DPM_REQUEST_EXIT_MODES);
		dpm_set_mode_exit_request(port);
		return true;
	} else if (PE_CHK_DPM_REQUEST(port, DPM_REQUEST_GET_SNK_CAPS)) {
		pe_set_dpm_curr_request(port, DPM_REQUEST_GET_SNK_CAPS);
		set_state_pe(port, PE_DR_GET_SINK_CAP);
		return true;
	} else if (PE_CHK_DPM_REQUEST(port,
				      DPM_REQUEST_SOP_PRIME_SOFT_RESET_SEND)) {
		pe_set_dpm_curr_request(port,
					DPM_REQUEST_SOP_PRIME_SOFT_RESET_SEND);
		pe[port].tx_type = TCPCI_MSG_SOP_PRIME;
		set_state_pe(port, PE_VCS_CBL_SEND_SOFT_RESET);
		return true;
	} else if (PE_CHK_DPM_REQUEST(port, DPM_REQUEST_DR_SWAP)) {
		pe_set_dpm_curr_request(port, DPM_REQUEST_DR_SWAP);
		/* 6.3.9 DR_Swap Message in Revision 3.1, Version 1.3
		 * If there are any Active Modes between the Port Partners when
		 * a DR_Swap Message is a received, then a Hard Reset Shall be
		 * performed
		 */
		if (PE_CHK_FLAG(port, PE_FLAGS_MODAL_OPERATION))
			pe_set_hard_reset(port);
		else
			set_state_pe(port, PE_DRS_SEND_SWAP);
		return true;
	} else if (0/*IS_ENABLED(CONFIG_USB_PD_DATA_RESET_MSG)*/ &&
		   PE_CHK_DPM_REQUEST(port, DPM_REQUEST_DATA_RESET)) {
		if (!pe_should_send_data_reset(port)) {
			PE_CLR_DPM_REQUEST(port, DPM_REQUEST_DATA_RESET);
			dpm_data_reset_complete(port);
			return false;
		}

		pe_set_dpm_curr_request(port, DPM_REQUEST_DATA_RESET);
		if (pe[port].data_role == PD_ROLE_DFP)
			set_state_pe(port, PE_DDR_SEND_DATA_RESET);
		else
			set_state_pe(port, PE_UDR_SEND_DATA_RESET);
		return true;
	} else if (1/*IS_ENABLED(CONFIG_USB_PD_REV30)*/ &&
		   PE_CHK_DPM_REQUEST(port, DPM_REQUEST_GET_REVISION)) {
		if (prl_get_rev(port, TCPCI_MSG_SOP) < PD_REV30) {
			PE_CLR_DPM_REQUEST(port, DPM_REQUEST_GET_REVISION);
			return false;
		}
		pe_set_dpm_curr_request(port, DPM_REQUEST_GET_REVISION);
		set_state_pe(port, PE_GET_REVISION);
		return true;
	} else if (0/*IS_ENABLED(CONFIG_USB_PD_EXTENDED_MESSAGES)*/ &&
		   PE_CHK_DPM_REQUEST(port, DPM_REQUEST_SEND_ALERT)) {
		if (prl_get_rev(port, TCPCI_MSG_SOP) < PD_REV30) {
			PE_CLR_DPM_REQUEST(port, DPM_REQUEST_SEND_ALERT);
			return false;
		}
		pe_set_dpm_curr_request(port, DPM_REQUEST_SEND_ALERT);
		set_state_pe(port, PE_SEND_ALERT);
		return true;
	}

	return false;
}

/*
 * Handle sink-specific DPM requests
 *
 * Returns true if state was set and calling run state should now return.
 */
static bool sink_dpm_requests(int port)
{
	/*
	 * Ignore source specific requests:
	 *   DPM_REQUEST_GOTO_MIN
	 *   DPM_REQUEST_SRC_CAP_CHANGE,
	 *   DPM_REQUEST_SEND_PING
	 */
	PE_CLR_DPM_REQUEST(port, DPM_REQUEST_GOTO_MIN |
					 DPM_REQUEST_SRC_CAP_CHANGE |
					 DPM_REQUEST_SEND_PING);

	if (!pe[port].dpm_request)
		return false;

	PE_SET_FLAG(port, PE_FLAGS_LOCALLY_INITIATED_AMS);

	if (PE_CHK_DPM_REQUEST(port, DPM_REQUEST_PR_SWAP)) {
		pe_set_dpm_curr_request(port, DPM_REQUEST_PR_SWAP);
		set_state_pe(port, PE_PRS_SNK_SRC_SEND_SWAP);
		return true;
	} else if (PE_CHK_DPM_REQUEST(port, DPM_REQUEST_SOURCE_CAP)) {
		pe_set_dpm_curr_request(port, DPM_REQUEST_SOURCE_CAP);
		set_state_pe(port, PE_SNK_GET_SOURCE_CAP);
		return true;
	} else if (PE_CHK_DPM_REQUEST(port, DPM_REQUEST_NEW_POWER_LEVEL)) {
		pe_set_dpm_curr_request(port, DPM_REQUEST_NEW_POWER_LEVEL);
		set_state_pe(port, PE_SNK_SELECT_CAPABILITY);
		return true;
	} else if (PE_CHK_DPM_REQUEST(port, DPM_REQUEST_FRS_DET_ENABLE)) {
		pe_set_frs_enable(port, 1);

		/* Requires no state change, fall through to false */
		PE_CLR_DPM_REQUEST(port, DPM_REQUEST_FRS_DET_ENABLE);
	} else if (PE_CHK_DPM_REQUEST(port, DPM_REQUEST_FRS_DET_DISABLE)) {
		pe_set_frs_enable(port, 0);
		/* Restore a default port current limit */
		typec_select_src_current_limit_rp(port, CONFIG_USB_PD_PULLUP);

		/* Requires no state change, fall through to false */
		PE_CLR_DPM_REQUEST(port, DPM_REQUEST_FRS_DET_DISABLE);
	} else if (common_src_snk_dpm_requests(port)) {
		return true;
	} else if (PE_CHK_DPM_REQUEST(port, DPM_REQUEST_EPR_MODE_ENTRY)) {
		if (pe_snk_in_epr_mode(port)) {
			PE_CLR_DPM_REQUEST(port, DPM_REQUEST_EPR_MODE_ENTRY);
			CPRINTS("C%d: Already in EPR mode", port);
			return false;
		}

		if (!pe_snk_can_enter_epr_mode(port)) {
			PE_CLR_DPM_REQUEST(port, DPM_REQUEST_EPR_MODE_ENTRY);
			CPRINTS("C%d: Not allowed to enter EPR", port);
			return false;
		}

		pe_set_dpm_curr_request(port, DPM_REQUEST_EPR_MODE_ENTRY);
		pd_set_max_voltage(PD_MAX_VOLTAGE_MV);
		set_state_pe(port, PE_SNK_SEND_EPR_MODE_ENTRY);
		return true;
	} else if (PE_CHK_DPM_REQUEST(port, DPM_REQUEST_EPR_MODE_EXIT)) {
		if (!pe_snk_in_epr_mode(port)) {
			PE_CLR_DPM_REQUEST(port, DPM_REQUEST_EPR_MODE_EXIT);
			CPRINTS("C%d: Not in EPR mode", port);
			return false;
		}

		/*
		 * If we're already in an SPR contract, send an exit
		 * message. Figure 8-217.
		 */
		if (pe_in_spr_contract(port)) {
			pe_set_dpm_curr_request(port,
						DPM_REQUEST_EPR_MODE_EXIT);
			set_state_pe(port, PE_SNK_SEND_EPR_MODE_EXIT);
			return true;
		}

		/*
		 * Can't exit yet because we're still in EPR contract.
		 * Send an SPR RDO to negotiate an SPR contract.
		 * Keep DPM_REQUEST_EPR_MODE_EXIT so that we can retry.
		 */
		CPRINTS("C%d: Request SPR before EPR exit", port);
		pd_set_max_voltage(PD_MAX_SPR_VOLTAGE);
		pe_set_dpm_curr_request(port, DPM_REQUEST_NEW_POWER_LEVEL);
		set_state_pe(port, PE_SNK_SELECT_CAPABILITY);
		return true;
	} else {
		const uint32_t dpm_request = pe[port].dpm_request;

		CPRINTF("Unhandled DPM Request %x received\n", dpm_request);
		PE_CLR_DPM_REQUEST(port, dpm_request);
	}

	PE_CLR_FLAG(port, PE_FLAGS_LOCALLY_INITIATED_AMS);

	return false;
}

/* Get the previous PE state. */
static enum usb_pe_state get_last_state_pe(const int port)
{
	return pe[port].ctx.previous - &pe_states[0];
}

static void print_current_state(const int port)
{
	const char *mode = "";

	if (1/*IS_ENABLED(CONFIG_USB_PD_REV30)*/ && pe_in_frs_mode(port))
		mode = " FRS-MODE";

	if (IS_ENABLED(USB_PD_DEBUG_LABELS))
		CPRINTS_L1("C%d: %s%s", port,
			   pe_state_names[get_state_pe(port)], mode);
	else
		CPRINTS_L1("C%d: pe-st%d", port, get_state_pe(port));
}

static void send_source_cap(int port)
{
	const uint32_t *src_pdo;
	const int src_pdo_cnt = dpm_get_source_pdo(&src_pdo, port);

	if (src_pdo_cnt == 0) {
		/* No source capabilities defined, sink only */
		send_ctrl_msg(port, TCPCI_MSG_SOP, PD_CTRL_REJECT);
	}

	tx_emsg[port].len = src_pdo_cnt * 4;
	memcpy(tx_emsg[port].buf, (uint8_t *)src_pdo, tx_emsg[port].len);

	send_data_msg(port, TCPCI_MSG_SOP, PD_DATA_SOURCE_CAP);
}

/*
 * Request desired charge voltage from source.
 */
static void pe_send_request_msg(int port)
{
	uint32_t vpd_vdo = 0;
	uint32_t rdo;
	uint32_t curr_limit;
	uint32_t supply_voltage;
	enum pd_data_msg_type msg;

	/*
	 * If we are charging through a VPD, the requested voltage and current
	 * might need adjusting.
	 */
	if ((get_usb_pd_cable_type(port) == IDH_PTYPE_VPD) &&
	    is_vpd_ct_supported(port)) {
		union vpd_vdo vpd =
			pd_get_am_discovery(port, TCPCI_MSG_SOP_PRIME)
				->identity.product_t1.vpd;

		/* The raw vpd_vdo is passed to pd_build_request */
		vpd_vdo = vpd.raw_value;
	}

	/* Build and send request RDO */
	pd_build_request(vpd_vdo, &rdo, &curr_limit, &supply_voltage, port);

	CPRINTF("C%d: Req [%d] %dmV %dmA", port, RDO_POS(rdo), supply_voltage,
		curr_limit);
	if (rdo & RDO_CAP_MISMATCH)
		CPRINTF(" Mismatch");
	CPRINTF("\n");

	pe[port].curr_limit = curr_limit;
	pe[port].supply_voltage = supply_voltage;

	if (1/*IS_ENABLED(CONFIG_USB_PD_EPR)*/ && pe_snk_in_epr_mode(port)) {
		const uint32_t *src_caps = pd_get_src_caps(port);

		tx_emsg[port].len = 8;
		memcpy(tx_emsg[port].buf, (uint8_t *)&rdo, sizeof(rdo));
		memcpy(tx_emsg[port].buf + sizeof(rdo),
		       (uint8_t *)&src_caps[RDO_POS(rdo) - 1],
		       sizeof(src_caps[0]));
		msg = PD_DATA_EPR_REQUEST;
	} else {
		tx_emsg[port].len = 4;
		memcpy(tx_emsg[port].buf, (uint8_t *)&rdo, tx_emsg[port].len);
		msg = PD_DATA_REQUEST;
	}

	send_data_msg(port, TCPCI_MSG_SOP, msg);
}

static void pe_update_src_pdo_flags(int port, int pdo_cnt, uint32_t *pdos)
{
	/*
	 * Only parse PDO flags if type is fixed
	 *
	 * Note: From 6.4.1 Capabilities Message "The vSafe5V Fixed Supply
	 * Object Shall always be the first object." so hitting this condition
	 * would mean the partner is voilating spec.
	 */
	if ((pdos[0] & PDO_TYPE_MASK) != PDO_TYPE_FIXED)
		return;

	if (0/*IS_ENABLED(CONFIG_CHARGE_MANAGER)*/) {
		if (pd_can_charge_from_device(port, pdo_cnt, pdos)) {
			charge_manager_update_dualrole(port, CAP_DEDICATED);
		} else {
			charge_manager_update_dualrole(port, CAP_DUALROLE);
		}
	}
}

/*
 * Evaluate whether our PR role is in the middle of changing, meaning we our
 * current PR role is not the one we expect to have very shortly.
 */
bool pe_is_pr_swapping(int port)
{
	enum usb_pe_state cur_state = get_state_pe(port);

	if (cur_state == PE_PRS_SRC_SNK_EVALUATE_SWAP ||
	    cur_state == PE_PRS_SRC_SNK_TRANSITION_TO_OFF ||
	    cur_state == PE_PRS_SNK_SRC_EVALUATE_SWAP ||
	    cur_state == PE_PRS_SNK_SRC_TRANSITION_TO_OFF)
		return true;

	return false;
}

void pd_request_power_swap(int port)
{
	/* Ignore requests when the board does not wish to swap */
	if (!pd_check_power_swap(port))
		return;

	/* Ignore requests when our power role is transitioning */
	if (pe_is_pr_swapping(port))
		return;

	/*
	 * Always reset the SRC to SNK PR swap counter when a PR swap is
	 * requested by policy.
	 */
	pe[port].src_snk_pr_swap_counter = 0;
	pd_dpm_request(port, DPM_REQUEST_PR_SWAP);
}

/* The function returns true if there is a PE state change, false otherwise */
static bool port_try_vconn_swap_on(int port)
{
	if (pe[port].vconn_swap_counter < N_VCONN_SWAP_COUNT) {
		pe_set_requested_vconn_role(port, PD_ROLE_VCONN_SRC);
		pd_dpm_request(port, DPM_REQUEST_VCONN_SWAP);
		set_state_pe(port, get_last_state_pe(port));
		return true;
	}

	CPRINTS("C%d: VCONN Swap counter exhausted", port);
	return false;
}

/*
 * Run discovery at our leisure from PE_SNK_Ready or PE_SRC_Ready, after
 * attempting to get into the desired default policy of DFP/Vconn source
 *
 * Return indicates whether set_state was called, in which case the calling
 * function should return as well.
 */
__maybe_unused static bool pe_attempt_port_discovery(int port)
{
	if (!0/*IS_ENABLED(CONFIG_USB_PD_ALT_MODE_DFP)*/)
		assert(0);

	/* TODO(b/272827504): Gate discovery via a DPM request and remove this
	 * flag.
	 */
	if (PE_CHK_FLAG(port, PE_FLAGS_DISCOVERY_DISABLED))
		return false;

	/* Apply Port Discovery DR Swap Policy */
	if (port_discovery_dr_swap_policy(
		    port, pe[port].data_role,
		    PE_CHK_FLAG(port, PE_FLAGS_DR_SWAP_TO_DFP))) {
		PE_SET_FLAG(port, PE_FLAGS_LOCALLY_INITIATED_AMS);
		PE_CLR_FLAG(port, PE_FLAGS_DR_SWAP_TO_DFP);

		pd_dpm_request(port, DPM_REQUEST_DR_SWAP);
		return false;
	}

	/*
	 * An edge case of DR Swap fail (port still UFP) and partner in PD 2.0.
	 * PD 2.0 allows only DFP to initiate Discover Identity, but partner may
	 * reject a DR Swap.
	 */
	if (pe[port].data_role == PD_ROLE_UFP &&
	    prl_get_rev(port, TCPCI_MSG_SOP) == PD_REV20) {
		/* Although, logically, "pd_disable_discovery" should set
		 * "PE_FLAGS_DISCOVERY_DISABLED," set it here to make its
		 * limited purpose obvious: When discovery is impossible, send
		 * discovery-done events exactly once.
		 */
		PE_SET_FLAG(port, PE_FLAGS_DISCOVERY_DISABLED);
		pd_disable_discovery(port);
		pd_notify_event(port, PD_STATUS_EVENT_SOP_DISC_DONE);
		pd_notify_event(port, PD_STATUS_EVENT_SOP_PRIME_DISC_DONE);
		return false;
	}

	/*
	 * Run discovery functions when the timer indicating either cable
	 * discovery spacing or BUSY spacing runs out.
	 */
	if (pd_timer_is_expired(port, PE_TIMER_DISCOVER_IDENTITY)) {
		if (pd_get_identity_discovery(port, TCPCI_MSG_SOP_PRIME) ==
		    PD_DISC_NEEDED) {
			pe[port].tx_type = TCPCI_MSG_SOP_PRIME;
			set_state_pe(port, PE_VDM_IDENTITY_REQUEST_CBL);
			return true;
		} else if (pd_get_identity_discovery(port, TCPCI_MSG_SOP) ==
				   PD_DISC_NEEDED &&
			   pe_can_send_sop_vdm(port, CMD_DISCOVER_IDENT)) {
			pe[port].tx_type = TCPCI_MSG_SOP;
			set_state_pe(port, PE_INIT_PORT_VDM_IDENTITY_REQUEST);
			return true;
		} else if (pd_get_svids_discovery(port, TCPCI_MSG_SOP) ==
				   PD_DISC_NEEDED &&
			   pe_can_send_sop_vdm(port, CMD_DISCOVER_SVID)) {
			pe[port].tx_type = TCPCI_MSG_SOP;
			set_state_pe(port, PE_INIT_VDM_SVIDS_REQUEST);
			return true;
		} else if (pd_get_modes_discovery(port, TCPCI_MSG_SOP) ==
				   PD_DISC_NEEDED &&
			   pe_can_send_sop_vdm(port, CMD_DISCOVER_MODES)) {
			pe[port].tx_type = TCPCI_MSG_SOP;
			set_state_pe(port, PE_INIT_VDM_MODES_REQUEST);
			return true;
		} else if (pd_get_svids_discovery(port, TCPCI_MSG_SOP_PRIME) ==
			   PD_DISC_NEEDED) {
			pe[port].tx_type = TCPCI_MSG_SOP_PRIME;
			set_state_pe(port, PE_INIT_VDM_SVIDS_REQUEST);
			return true;
		} else if (pd_get_modes_discovery(port, TCPCI_MSG_SOP_PRIME) ==
			   PD_DISC_NEEDED) {
			pe[port].tx_type = TCPCI_MSG_SOP_PRIME;
			set_state_pe(port, PE_INIT_VDM_MODES_REQUEST);
			return true;
		} else {
			pd_timer_disable(port, PE_TIMER_DISCOVER_IDENTITY);
			return false;
		}
	}

	return false;
}

bool pd_setup_vdm_request(int port, enum tcpci_msg_type tx_type, uint32_t *vdm,
			  uint32_t vdo_cnt)
{
	if (vdo_cnt < VDO_HDR_SIZE || vdo_cnt > VDO_MAX_SIZE)
		return false;

	pe[port].tx_type = tx_type;
	memcpy(pe[port].vdm_data, vdm, vdo_cnt * sizeof(*vdm));
	pe[port].vdm_cnt = vdo_cnt;

	return true;
}

/*
 * This function must only be called from the PE_SNK_READY entry and
 * PE_SRC_READY entry State.
 *
 * TODO(b:181339670) Rethink jitter timer restart if this is the first
 * message but the partner gets a message in first, may not want to
 * disable and restart it.
 */
static void pe_update_wait_and_add_jitter_timer(int port)
{
	/*
	 * In PD2.0 Mode
	 *
	 * For Source:
	 * Give the sink some time to send any messages
	 * before we may send messages of our own.  Add
	 * some jitter of up to ~345ms, to prevent
	 * multiple collisions. This delay also allows
	 * the sink device to request power role swap
	 * and allow the the accept message to be sent
	 * prior to CMD_DISCOVER_IDENT being sent in the
	 * SRC_READY state.
	 *
	 * For Sink:
	 * Give the source some time to send any messages before
	 * we start our interrogation.  Add some jitter of up to
	 * ~345ms to prevent multiple collisions.
	 */
	if (prl_get_rev(port, TCPCI_MSG_SOP) == PD_REV20 &&
	    PE_CHK_FLAG(port, PE_FLAGS_FIRST_MSG) &&
	    pd_timer_is_disabled(port, PE_TIMER_WAIT_AND_ADD_JITTER)) {
		pd_timer_enable(port, PE_TIMER_WAIT_AND_ADD_JITTER,
				SRC_SNK_READY_HOLD_OFF_US +
					(get_time().le.lo & 0xf) * 23 * MSEC);
	}
}

/**
 * Common sender response message handling
 *
 * This is setup like a pseudo state machine parent state.  It
 * centralizes the SenderResponseTimer for the calling states, as
 * well as checking message send status.
 */
/*
 * pe_sender_response_msg_entry
 * Initialization for handling sender response messages.
 *
 * @param port USB-C Port number
 */
static void pe_sender_response_msg_entry(const int port)
{
	/* Stop sender response timer */
	pd_timer_disable(port, PE_TIMER_SENDER_RESPONSE);
}

/*
 * pe_sender_response_msg_run
 * Check status of sender response messages.
 *
 * The normal progression of pe_sender_response_msg_entry is:
 *    PENDING -> (COMPLETED/SENT) -> SENT -> SENT ...
 * or
 *    PENDING -> DISCARDED
 *    PENDING -> DPM_DISCARDED
 *
 * NOTE: it is not valid to call this function for a message after
 * receiving either PE_MSG_DISCARDED or PE_MSG_DPM_DISCARDED until
 * another message has been sent and pe_sender_response_msg_entry is called
 * again.
 *
 * @param port USB-C Port number
 * @return the current pe_msg_check
 */
static enum pe_msg_check pe_sender_response_msg_run(const int port)
{
	timestamp_t tx_success_ts;
	uint32_t offset;
	if (pd_timer_is_disabled(port, PE_TIMER_SENDER_RESPONSE)) {
		/* Check for Discard */
		if (PE_CHK_FLAG(port, PE_FLAGS_MSG_DISCARDED)) {
			int dpm_request = pe[port].dpm_curr_request;

			PE_CLR_FLAG(port, PE_FLAGS_MSG_DISCARDED);
			/* Restore the DPM Request */
			if (dpm_request) {
				PE_SET_DPM_REQUEST(port, dpm_request);
				return PE_MSG_DPM_DISCARDED;
			}
			return PE_MSG_DISCARDED;
		}

		/* Check for GoodCRC */
		if (PE_CHK_FLAG(port, PE_FLAGS_TX_COMPLETE)) {
			PE_CLR_FLAG(port, PE_FLAGS_TX_COMPLETE);

			/* TCPC TX success time stamp */
			tx_success_ts = prl_get_tcpc_tx_success_ts(port);
			/* Calculate the delay from TX success to PE */
			offset = time_since32(tx_success_ts);

			/*
			 * Initialize and run the SenderResponseTimer by
			 * offsetting it with TX transmit success time.
			 * This would remove the effect of the latency from
			 * propagating the TX status.
			 */
			pd_timer_enable(port, PE_TIMER_SENDER_RESPONSE,
					PD_T_SENDER_RESPONSE - offset);
			return PE_MSG_SEND_COMPLETED;
		}
		return PE_MSG_SEND_PENDING;
	}
	return PE_MSG_SENT;
}

/*
 * pe_sender_response_msg_exit
 * Exit cleanup for handling sender response messages.
 *
 * @param port USB-C Port number
 */
static void pe_sender_response_msg_exit(int port)
{
	pd_timer_disable(port, PE_TIMER_SENDER_RESPONSE);
}

/*
 * Transitions state after receiving a Not Supported extended message. Under
 * appropriate conditions, transitions to a PE_{SRC,SNK}_Chunk_Received.
 */
static void extended_message_not_supported(int port, uint32_t *payload)
{
	uint16_t ext_header = GET_EXT_HEADER(*payload);

	if (1/*IS_ENABLED(CONFIG_USB_PD_REV30)*/ &&
	    !0/*IS_ENABLED(CONFIG_USB_PD_EXTENDED_MESSAGES)*/ &&
	    PD_EXT_HEADER_CHUNKED(ext_header) &&
	    PD_EXT_HEADER_DATA_SIZE(ext_header) >
		    PD_MAX_EXTENDED_MSG_CHUNK_LEN) {
		set_state_pe(port, pe[port].power_role == PD_ROLE_SOURCE ?
					   PE_SRC_CHUNK_RECEIVED :
					   PE_SNK_CHUNK_RECEIVED);
		return;
	}

	set_state_pe(port, PE_SEND_NOT_SUPPORTED);
}

/**
 * PE_SNK_Startup State
 */
static void pe_snk_startup_entry(int port)
{
	print_current_state(port);

	/* Reset the protocol layer */
	prl_reset_soft(port);

	/* Set initial data role */
	pe[port].data_role = pd_get_data_role(port);

	/* Set initial power role */
	pe[port].power_role = PD_ROLE_SINK;

	/* Invalidate explicit contract */
	pe_invalidate_explicit_contract(port);

	/* Clear any pending PD events */
	pe_clear_ado(port);

	if (PE_CHK_FLAG(port, PE_FLAGS_PR_SWAP_COMPLETE)) {
		PE_CLR_FLAG(port, PE_FLAGS_PR_SWAP_COMPLETE);
		/*
		 * Protocol layer reset clears the message IDs for all SOP
		 * types. Indicate that a SOP' soft reset is required before any
		 * other messages are sent to the cable.
		 *
		 * Note that other paths into this state are for the initial
		 * connection and for a hard reset. In both cases the cable
		 * should also automatically clear the message IDs so don't
		 * generate an SOP' soft reset for those cases. Sending
		 * unnecessary SOP' soft resets causes bad behavior with
		 * some devices. See b/179325862.
		 */
		pd_dpm_request(port, DPM_REQUEST_SOP_PRIME_SOFT_RESET_SEND);

		/*
		 * Some port partners may violate spec and attempt to
		 * communicate with the cable after power role swaps, despite
		 * not being Vconn source.  Disable our SOP' receiving here to
		 * avoid GoodCRC-ing any erroneous cable probes, and re-enable
		 * after our contract is in place.
		 */
		if (tc_is_vconn_src(port))
			tcpm_sop_prime_enable(port, false);

		dpm_remove_sink(port);
	} else {
		/*
		 * Set DiscoverIdentityTimer to trigger when we enter
		 * snk_ready for the first time.
		 */
		pd_timer_enable(port, PE_TIMER_DISCOVER_IDENTITY, 0);

		/* Clear port discovery/mode flags */
		pd_dfp_discovery_init(port);
		pd_dfp_mode_init(port);
		dpm_init(port);
		pe[port].discover_identity_counter = 0;

		/* Reset dr swap attempt counter */
		pe[port].dr_swap_attempt_counter = 0;

		/* Reset VCONN swap counter */
		pe[port].vconn_swap_counter = 0;
		/*
		 * TODO: POLICY decision:
		 * Mark that we'd like to try being Vconn source and DFP
		 */
		PE_SET_FLAG(port, PE_FLAGS_DR_SWAP_TO_DFP);
	}

	/*
	 * Request sink caps for FRS, output power consideration, or reporting
	 * to the AP through host commands.
	 *
	 * On entry to the PE_SNK_Ready state if the Sink supports Fast Role
	 * Swap, then the Policy Engine Shall do the following:
	 * - Send a Get_Sink_Cap Message
	 */
	if (0/*IS_ENABLED(CONFIG_USB_PD_HOST_CMD)*/ || CONFIG_USB_PD_3A_PORTS > 0 ||
	    0/*IS_ENABLED(CONFIG_USB_PD_FRS)*/)
		pd_dpm_request(port, DPM_REQUEST_GET_SNK_CAPS);

	/*
	 * Request partner's revision information. The PE_Get_Revision
	 * state will only send Get_Revision to partners with major
	 * revision 3.0
	 */
	pd_dpm_request(port, DPM_REQUEST_GET_REVISION);
}

static void pe_snk_startup_run(int port)
{
	/* Wait until protocol layer is running */
	if (!prl_is_running(port))
		return;

	/*
	 * Once the reset process completes, the Policy Engine Shall
	 * transition to the PE_SNK_Discovery state
	 */
	set_state_pe(port, PE_SNK_DISCOVERY);
}

/**
 * PE_SNK_Discovery State
 */
static void pe_snk_discovery_entry(int port)
{
	print_current_state(port);
}

static void pe_snk_discovery_run(int port)
{
	/*
	 * Transition to the PE_SNK_Wait_for_Capabilities state when:
	 *   1) VBUS has been detected
	 */
	if (!pd_check_vbus_level(port, VBUS_REMOVED))
		set_state_pe(port, PE_SNK_WAIT_FOR_CAPABILITIES);
}

/**
 * PE_SNK_Wait_For_Capabilities State
 */
static void pe_snk_wait_for_capabilities_entry(int port)
{
	print_current_state(port);

	/* Initialize and start the SinkWaitCapTimer */
	pd_timer_enable(port, PE_TIMER_TIMEOUT, PD_T_SINK_WAIT_CAP);
}

static void pe_snk_wait_for_capabilities_run(int port)
{
	uint8_t type;
	uint8_t cnt;
	uint8_t ext;
	uint32_t *payload;

	/*
	 * Transition to the PE_SNK_Evaluate_Capability state when:
	 *  1) A Source_Capabilities Message is received.
	 */
	if (PE_CHK_FLAG(port, PE_FLAGS_MSG_RECEIVED)) {
		PE_CLR_FLAG(port, PE_FLAGS_MSG_RECEIVED);

		type = PD_HEADER_TYPE(rx_emsg[port].header);
		cnt = PD_HEADER_CNT(rx_emsg[port].header);
		ext = PD_HEADER_EXT(rx_emsg[port].header);
		payload = (uint32_t *)rx_emsg[port].buf;

		if ((ext == 0) && (cnt > 0) && (type == PD_DATA_SOURCE_CAP)) {
			set_state_pe(port, PE_SNK_EVALUATE_CAPABILITY);
			return;
		} else if (ext > 0) {
			switch (type) {
			case PD_EXT_EPR_SOURCE_CAP:
				if (!pe_snk_in_epr_mode(port))
					break;
				set_state_pe(port, PE_SNK_EVALUATE_CAPABILITY);
				break;
			default:
				extended_message_not_supported(port, payload);
			}
			return;
		}
	}

	/* When the SinkWaitCapTimer times out, perform a Hard Reset. */
	if (pd_timer_is_expired(port, PE_TIMER_TIMEOUT)) {
		PE_SET_FLAG(port, PE_FLAGS_SNK_WAIT_CAP_TIMEOUT);
		pe_set_hard_reset(port);
	}
}

static void pe_snk_wait_for_capabilities_exit(int port)
{
	pd_timer_disable(port, PE_TIMER_TIMEOUT);
}

/**
 * PE_SNK_Evaluate_Capability State
 */
static void pe_snk_evaluate_capability_entry(int port)
{
	uint32_t *pdo = (uint32_t *)rx_emsg[port].buf;
	uint32_t num = rx_emsg[port].len >> 2;

	print_current_state(port);

	/* Reset Hard Reset counter to zero */
	pe[port].hard_reset_counter = 0;

	/* Set to highest revision supported by both ports. */
	prl_set_rev(port, TCPCI_MSG_SOP,
		    MIN(PD_REVISION, PD_HEADER_REV(rx_emsg[port].header)));

	init_cable_rev(port);

	/* Parse source caps if they have changed */
	if (pe[port].src_cap_cnt != num ||
	    memcmp(pdo, pe[port].src_caps, num << 2)) {
		/*
		 * If port policy preference is to be a power role source,
		 * then request a power role swap.  If we'd previously queued a
		 * PR swap but can now charge from this device, clear it.
		 */
		if (!pd_can_charge_from_device(port, num, pdo))
			pd_request_power_swap(port);
		else
			PE_CLR_DPM_REQUEST(port, DPM_REQUEST_PR_SWAP);
	}

	pe_update_src_pdo_flags(port, num, pdo);
	pd_set_src_caps(port, num, pdo);

	/* Evaluate the options based on supplied capabilities */
	pd_process_source_cap(port, pe[port].src_cap_cnt, pe[port].src_caps);

	/* Device Policy Response Received */
	set_state_pe(port, PE_SNK_SELECT_CAPABILITY);

#ifdef HAS_TASK_DPS
	/* Wake DPS task to evaluate the SrcCaps */
	task_wake(TASK_ID_DPS);
#endif
}

/**
 * PE_SNK_Select_Capability State
 */
static void pe_snk_select_capability_entry(int port)
{
	print_current_state(port);

	/* Send Request */
	pe_send_request_msg(port);
	pe_sender_response_msg_entry(port);

	/* We are PD Connected */
	PE_SET_FLAG(port, PE_FLAGS_PD_CONNECTION);
	tc_pd_connection(port, 1);
}

static void pe_snk_apply_psnkstdby(int port)
{
	uint32_t mv = pd_get_requested_voltage(port);
	uint32_t high;

	/*
	 * Apply 2.5W ceiling during transition. We need choose the larger of
	 * the current input voltage and the new PDO voltage because during a
	 * transition, both voltages can be applied to the device. If the
	 * current source isn't PD, we don't need to care about drawing more
	 * than pSnkStdby. Thus, it's not considered (in the else clause).
	 */
	if (charge_manager_get_supplier() == CHARGE_SUPPLIER_PD)
		high = MAX(charge_manager_get_charger_voltage(), mv);
	else
		high = mv;
	charge_manager_force_ceil(
		port, high > 0 ? PD_SNK_STDBY_MW * 1000 / high : PD_MIN_MA);
}

static void pe_snk_select_capability_run(int port)
{
	uint8_t type;
	uint8_t cnt;
	enum tcpci_msg_type sop;
	enum pe_msg_check msg_check;

	/*
	 * Check the state of the message sent
	 */
	msg_check = pe_sender_response_msg_run(port);

	/*
	 * Handle discarded message
	 */
	if (msg_check & PE_MSG_DISCARDED) {
		/*
		 * The sent REQUEST message was discarded.  This can be at
		 * the start of an AMS or in the middle.  Handle what to
		 * do based on where we came from.
		 * 1) SE_SNK_EVALUATE_CAPABILITY: sends SoftReset
		 * 2) SE_SNK_READY: goes back to SNK Ready
		 */
		if (get_last_state_pe(port) == PE_SNK_EVALUATE_CAPABILITY)
			pe_send_soft_reset(port, TCPCI_MSG_SOP);
		else
			set_state_pe(port, PE_SNK_READY);
		return;
	}

	if ((msg_check & PE_MSG_SENT) &&
	    PE_CHK_FLAG(port, PE_FLAGS_MSG_RECEIVED)) {
		PE_CLR_FLAG(port, PE_FLAGS_MSG_RECEIVED);
		type = PD_HEADER_TYPE(rx_emsg[port].header);
		cnt = PD_HEADER_CNT(rx_emsg[port].header);
		sop = PD_HEADER_GET_SOP(rx_emsg[port].header);

		/*
		 * Transition to the PE_SNK_Transition_Sink state when:
		 *  1) An Accept Message is received from the Source.
		 *
		 * Transition to the PE_SNK_Wait_for_Capabilities state when:
		 *  1) There is no Explicit Contract in place and
		 *  2) A Reject Message is received from the Source or
		 *  3) A Wait Message is received from the Source.
		 *
		 * Transition to the PE_SNK_Ready state when:
		 *  1) There is an Explicit Contract in place and
		 *  2) A Reject Message is received from the Source or
		 *  3) A Wait Message is received from the Source.
		 *
		 * Transition to the PE_SNK_Hard_Reset state when:
		 *  1) A SenderResponseTimer timeout occurs.
		 */

		/* Only look at control messages */
		if (cnt == 0) {
			/*
			 * Accept Message Received
			 */
			if (type == PD_CTRL_ACCEPT) {
				/* explicit contract is now in place */
				pe_set_explicit_contract(port);

				if (0/*IS_ENABLED(CONFIG_CHARGE_MANAGER)*/)
					pe_snk_apply_psnkstdby(port);

				set_state_pe(port, PE_SNK_TRANSITION_SINK);

				return;
			}
			/*
			 * Reject or Wait Message Received
			 */
			else if (type == PD_CTRL_REJECT ||
				 type == PD_CTRL_WAIT) {
				if (type == PD_CTRL_WAIT)
					PE_SET_FLAG(port, PE_FLAGS_WAIT);

				pd_timer_disable(port, PE_TIMER_SINK_REQUEST);

				/*
				 * We had a previous explicit contract, so
				 * transition to PE_SNK_Ready
				 */
				if (PE_CHK_FLAG(port,
						PE_FLAGS_EXPLICIT_CONTRACT))
					set_state_pe(port, PE_SNK_READY);
				/*
				 * No previous explicit contract, so transition
				 * to PE_SNK_Wait_For_Capabilities
				 */
				else
					set_state_pe(
						port,
						PE_SNK_WAIT_FOR_CAPABILITIES);
				return;
			}
			/*
			 * Unexpected Control Message Received
			 */
			else {
				/* Send Soft Reset */
				pe_send_soft_reset(port, sop);
				return;
			}
		}
		/*
		 * Unexpected Data Message
		 */
		else {
			/* Send Soft Reset */
			pe_send_soft_reset(port, sop);
			return;
		}
	}

	/* SenderResponsetimer timeout */
	if (pd_timer_is_expired(port, PE_TIMER_SENDER_RESPONSE))
		pe_set_hard_reset(port);
}

void pe_snk_select_capability_exit(int port)
{
	pe_sender_response_msg_exit(port);
}

/**
 * PE_SNK_Transition_Sink State
 */
static void pe_snk_transition_sink_entry(int port)
{
	print_current_state(port);

	/* Initialize and run PSTransitionTimer */
	pd_timer_enable(port, PE_TIMER_PS_TRANSITION, PD_T_PS_TRANSITION);
}

static void pe_snk_transition_sink_run(int port)
{
	/*
	 * Transition to the PE_SNK_Ready state when:
	 *  1) A PS_RDY Message is received from the Source.
	 *
	 * Transition to the PE_SNK_Hard_Reset state when:
	 *  1) A Protocol Error occurs.
	 */

	if (PE_CHK_FLAG(port, PE_FLAGS_MSG_RECEIVED)) {
		PE_CLR_FLAG(port, PE_FLAGS_MSG_RECEIVED);

		/*
		 * PS_RDY message received
		 */
		if ((PD_HEADER_CNT(rx_emsg[port].header) == 0) &&
		    (PD_HEADER_TYPE(rx_emsg[port].header) == PD_CTRL_PS_RDY)) {
			/*
			 * Set first message flag to trigger a wait and add
			 * jitter delay when operating in PD2.0 mode.
			 */
			PE_SET_FLAG(port, PE_FLAGS_FIRST_MSG);
			pd_timer_disable(port, PE_TIMER_WAIT_AND_ADD_JITTER);

			/*
			 * If we've successfully completed our new power
			 * contract, ensure SOP' communication is enabled before
			 * entering PE_SNK_READY.  It may have been disabled
			 * during a power role swap to avoid interoperability
			 * issues with out-of-spec partners.
			 */
			if (tc_is_vconn_src(port))
				tcpm_sop_prime_enable(port, true);

			/*
			 * Evaluate port's sink caps for FRS current, if
			 * already available
			 */
			if (pd_get_snk_cap_cnt(port) > 0)
				dpm_evaluate_sink_fixed_pdo(
					port, *pd_get_snk_caps(port));

			/*
			 * Per PD r3.1 v1.8 ss 8.3.3.3.6, the PE should start
			 * actually sinking according to the new power contract
			 * upon exit from PE_SNK_Transition_Sink. Setting the
			 * current limit here in the run function instead of the
			 * exit function ensures that this happens before the
			 * next run of the type-C state machine. This avoids a
			 * race condition in the case where the TC transitions
			 * to Unattached immediately after contract negotiation.
			 * In this case, the TC sets the current limit to 0, and
			 * this should happen last.
			 */
			pd_set_input_current_limit(port, pe[port].curr_limit,
						   pe[port].supply_voltage);
			if (0/*IS_ENABLED(CONFIG_CHARGE_MANAGER)*/)
				/* Set ceiling based on what's negotiated */
				charge_manager_set_ceil(port, CEIL_REQUESTOR_PD,
							pe[port].curr_limit);
			set_state_pe(port, PE_SNK_READY);
		} else {
			/*
			 * Protocol Error
			 */
			pe_set_hard_reset(port);
		}
		return;
	}

	/*
	 * Timeout will lead to a Hard Reset
	 */
	if (pd_timer_is_expired(port, PE_TIMER_PS_TRANSITION) &&
	    pe[port].hard_reset_counter <= N_HARD_RESET_COUNT) {
		PE_SET_FLAG(port, PE_FLAGS_PS_TRANSITION_TIMEOUT);

		pe_set_hard_reset(port);
	}
}

static void pe_snk_transition_sink_exit(int port)
{
	pd_timer_disable(port, PE_TIMER_PS_TRANSITION);

	if (IS_ENABLED(CONFIG_USB_PD_DPS))
		if (charge_manager_get_active_charge_port() == port)
			dps_update_stabilized_time(port);
}

/**
 * PE_SNK_Ready State
 */
static void pe_snk_ready_entry(int port)
{
	if (get_last_state_pe(port) != PE_SNK_EPR_KEEP_ALIVE) {
		print_current_state(port);
	}

	/* Ensure any message send flags are cleaned up */
	PE_CLR_MASK(port, PE_MASK_READY_CLR);

	/* If configured, clear any stale hard reset events */
	if (IS_ENABLED(CONFIG_USB_PD_CLEAR_HARD_RESET_STATUS))
		pd_clear_events(port, PD_STATUS_EVENT_HARD_RESET);

	/* Clear DPM Current Request */
	pe[port].dpm_curr_request = 0;

	/*
	 * On entry to the PE_SNK_Ready state as the result of a wait,
	 * then do the following:
	 *   1) Initialize and run the SinkRequestTimer
	 */
	if (PE_CHK_FLAG(port, PE_FLAGS_WAIT)) {
		PE_CLR_FLAG(port, PE_FLAGS_WAIT);
		pd_timer_enable(port, PE_TIMER_SINK_REQUEST, PD_T_SINK_REQUEST);
	}

	/*
	 * Wait and add jitter if we are operating in PD2.0 mode and no messages
	 * have been sent since enter this state.
	 */
	pe_update_wait_and_add_jitter_timer(port);

	if (1/*IS_ENABLED(CONFIG_USB_PD_EPR)*/) {
		if (pe_snk_in_epr_mode(port))
			pd_timer_enable(port, PE_TIMER_SINK_EPR_KEEP_ALIVE,
					PD_T_SINK_EPR_KEEP_ALIVE);
		else if (!PE_CHK_FLAG(port, PE_FLAGS_EPR_EXPLICIT_EXIT))
			pd_dpm_request(port, DPM_REQUEST_EPR_MODE_ENTRY);
	}
}

static void pe_snk_ready_run(int port)
{
	/*
	 * Handle incoming messages before discovery and DPMs other than hard
	 * reset
	 */
	if (PE_CHK_FLAG(port, PE_FLAGS_MSG_RECEIVED)) {
		uint8_t type = PD_HEADER_TYPE(rx_emsg[port].header);
		uint8_t cnt = PD_HEADER_CNT(rx_emsg[port].header);
		uint8_t ext = PD_HEADER_EXT(rx_emsg[port].header);
		uint32_t *payload = (uint32_t *)rx_emsg[port].buf;

		PE_CLR_FLAG(port, PE_FLAGS_MSG_RECEIVED);

		/* Extended Message Request */
		if (ext > 0) {
			switch (type) {
			default:
				extended_message_not_supported(port, payload);
			}
			return;
		}
		/* Data Messages */
		else if (cnt > 0) {
			switch (type) {
			case PD_DATA_SOURCE_CAP:
				set_state_pe(port, PE_SNK_EVALUATE_CAPABILITY);
				return;
			case PD_DATA_VENDOR_DEF:
				if (PD_VDO_SVDM(*payload))
					set_state_pe(port, PE_VDM_RESPONSE);
				/* The TCPM does not support any unstructured
				 * VDMs. For PD 3.x, send Not Supported. For
				 * PD 2.0, ignore.
				 */
				else if (prl_get_rev(port, TCPCI_MSG_SOP) >
					 PD_REV20)
					set_state_pe(port,
						     PE_SEND_NOT_SUPPORTED);
				return;
			case PD_DATA_BIST:
				set_state_pe(port, PE_BIST_TX);
				return;
			case PD_DATA_ALERT:
				set_state_pe(port, PE_ALERT_RECEIVED);
				return;
			case PD_DATA_EPR_MODE:
				struct eprmdo *mdo = (void *)payload;

				if (mdo->action == PD_EPRMDO_ACTION_EXIT) {
					set_state_pe(
						port,
						PE_SNK_EPR_MODE_EXIT_RECEIVED);
				}
				return;
			default:
				set_state_pe(port, PE_SEND_NOT_SUPPORTED);
				return;
			}
		}
		/* Control Messages */
		else {
			switch (type) {
			case PD_CTRL_GOOD_CRC:
				/* Do nothing */
				break;
			case PD_CTRL_PING:
				/* Do nothing */
				break;
			case PD_CTRL_GET_SOURCE_CAP:
				set_state_pe(port, PE_DR_SNK_GIVE_SOURCE_CAP);
				return;
			case PD_CTRL_GET_SINK_CAP:
				set_state_pe(port, PE_SNK_GIVE_SINK_CAP);
				return;
			case PD_CTRL_GOTO_MIN:
				set_state_pe(port, PE_SNK_TRANSITION_SINK);
				return;
			case PD_CTRL_PR_SWAP:
				set_state_pe(port,
					     PE_PRS_SNK_SRC_EVALUATE_SWAP);
				return;
			case PD_CTRL_DR_SWAP:
				if (PE_CHK_FLAG(port, PE_FLAGS_MODAL_OPERATION))
					pe_set_hard_reset(port);
				else
					set_state_pe(port,
						     PE_DRS_EVALUATE_SWAP);
				return;
			case PD_CTRL_VCONN_SWAP:
				if (0/*IS_ENABLED(CONFIG_USBC_VCONN)*/)
					set_state_pe(port,
						     PE_VCS_EVALUATE_SWAP);
				else
					set_state_pe(port,
						     PE_SEND_NOT_SUPPORTED);
				return;
			case PD_CTRL_NOT_SUPPORTED:
				/* Do nothing */
				break;
			/*
			 * USB PD 3.0 6.8.1:
			 * Receiving an unexpected message shall be responded
			 * to with a soft reset message.
			 */
			case PD_CTRL_ACCEPT:
			case PD_CTRL_REJECT:
			case PD_CTRL_WAIT:
			case PD_CTRL_PS_RDY:
				pe_send_soft_reset(
					port, PD_HEADER_GET_SOP(
						      rx_emsg[port].header));
				return;
			/*
			 * Receiving an unknown or unsupported message
			 * shall be responded to with a not supported message.
			 */
			default:
				set_state_pe(port, PE_SEND_NOT_SUPPORTED);
				return;
			}
		}
	}

	/*
	 * Make sure the PRL layer isn't busy with receiving or transmitting
	 * chunked messages before attempting to transmit a new message.
	 */
	if (prl_is_busy(port))
		return;

	if (PE_CHK_FLAG(port, PE_FLAGS_VDM_REQUEST_CONTINUE)) {
		PE_CLR_FLAG(port, PE_FLAGS_VDM_REQUEST_CONTINUE);
		set_state_pe(port, PE_VDM_REQUEST_DPM);
		return;
	}

	if (pd_timer_is_disabled(port, PE_TIMER_WAIT_AND_ADD_JITTER) ||
	    pd_timer_is_expired(port, PE_TIMER_WAIT_AND_ADD_JITTER)) {
		PE_CLR_FLAG(port, PE_FLAGS_FIRST_MSG);
		pd_timer_disable(port, PE_TIMER_WAIT_AND_ADD_JITTER);

		if (pd_timer_is_expired(port, PE_TIMER_SINK_REQUEST)) {
			pd_timer_disable(port, PE_TIMER_SINK_REQUEST);
			set_state_pe(port, PE_SNK_SELECT_CAPABILITY);
			return;
		}

		/*
		 * Handle Device Policy Manager Requests
		 */
		if (sink_dpm_requests(port))
			return;

		/*
		 * Attempt discovery if possible, and return if state was
		 * changed for that discovery.
		 */
		if (pe_attempt_port_discovery(port))
			return;

		/* Inform DPM state machine that PE is set for messages */
		dpm_set_pe_ready(port, true);

		if (1/*IS_ENABLED(CONFIG_USB_PD_EPR)*/ &&
		    pd_timer_is_expired(port, PE_TIMER_SINK_EPR_KEEP_ALIVE)) {
			set_state_pe(port, PE_SNK_EPR_KEEP_ALIVE);
			return;
		}
	}
}

static void pe_snk_ready_exit(int port)
{
	/* Inform DPM state machine that PE is in ready state */
	dpm_set_pe_ready(port, false);

	if (1/*IS_ENABLED(CONFIG_USB_PD_EPR)*/ && pe_snk_in_epr_mode(port)) {
		pd_timer_disable(port, PE_TIMER_SINK_EPR_KEEP_ALIVE);
	}
}

/**
 * PE_SNK_Hard_Reset
 */
static void pe_snk_hard_reset_entry(int port)
{

	print_current_state(port);

	/*
	 * Note: If the SinkWaitCapTimer times out and the HardResetCounter is
	 *       greater than nHardResetCount the Sink Shall assume that the
	 *       Source is non-responsive.
	 */
	if (PE_CHK_FLAG(port, PE_FLAGS_SNK_WAIT_CAP_TIMEOUT) &&
	    pe[port].hard_reset_counter > N_HARD_RESET_COUNT) {
		set_state_pe(port, PE_SRC_DISABLED);
		return;
	}

	/*
	 * If we're about to kill our active charge port and have no battery to
	 * supply power, disable the PE layer instead.  If we have no battery,
	 * but we haven't determined our active charge port yet, also avoid
	 * performing the HardReset.  It might be that this port was our active
	 * charge port.
	 *
	 * Note: On systems without batteries (ex. chromeboxes), it's preferable
	 * to brown out rather than leave the port only semi-functional for a
	 * customer.  For systems which should have a battery, this condition is
	 * not expected to be encountered by a customer.
	 */
	if (IS_ENABLED(CONFIG_BATTERY) && (battery_is_present() == BP_NO) &&
	    0/*IS_ENABLED(CONFIG_CHARGE_MANAGER)*/ &&
	    ((port == charge_manager_get_active_charge_port() ||
	      (charge_manager_get_active_charge_port() == CHARGE_PORT_NONE))) &&
	    system_get_reset_flags() & EC_RESET_FLAG_SYSJUMP) {
		CPRINTS("C%d: Disabling port to avoid brown out, "
			"please reboot EC to enable port again",
			port);
		set_state_pe(port, PE_SRC_DISABLED);
		return;
	}

	/*
	 * Workaround for power_state:rec with cros_ec_softrec_power on
	 * chromeboxes. If we're booted in recovery and about to reset our
	 * active charge port, preserve the ap-off and stay-in-ro flags so that
	 * the next boot after we brown out will still be recovery.
	 */
	if (IS_ENABLED(CONFIG_USB_PD_RESET_PRESERVE_RECOVERY_FLAGS) &&
	    port == charge_manager_get_active_charge_port() &&
	    (system_get_reset_flags() & EC_RESET_FLAG_STAY_IN_RO) &&
	    system_get_image_copy() == EC_IMAGE_RO) {
		CPRINTS("C%d: Preserve ap-off and stay-in-ro across PD reset",
			port);
		chip_save_reset_flags(chip_read_reset_flags() |
				      EC_RESET_FLAG_AP_OFF |
				      EC_RESET_FLAG_STAY_IN_RO);
	}


	PE_CLR_MASK(port, BIT(PE_FLAGS_SNK_WAIT_CAP_TIMEOUT_FN) |
				  BIT(PE_FLAGS_PROTOCOL_ERROR_FN));

	/* Request the generation of Hard Reset Signaling by the PHY Layer */
	prl_execute_hard_reset(port);

	/* Increment the HardResetCounter */
	pe[port].hard_reset_counter++;

	/*
	 * Transition the Sink’s power supply to the new power level if
	 * PSTransistionTimer timeout occurred.
	 */
	if (PE_CHK_FLAG(port, PE_FLAGS_PS_TRANSITION_TIMEOUT)) {
		PE_CLR_FLAG(port, PE_FLAGS_PS_TRANSITION_TIMEOUT);

		/* Transition Sink's power supply to the new power level */
		pd_set_input_current_limit(port, pe[port].curr_limit,
					   pe[port].supply_voltage);
		if (0/*IS_ENABLED(CONFIG_CHARGE_MANAGER)*/)
			/* Set ceiling based on what's negotiated */
			charge_manager_set_ceil(port, CEIL_REQUESTOR_PD,
						pe[port].curr_limit);
	}
}

static void pe_snk_hard_reset_run(int port)
{
	/*
	 * Transition to the PE_SNK_Transition_to_default state when:
	 *  1) The Hard Reset is complete.
	 */
	if (PE_CHK_FLAG(port, PE_FLAGS_HARD_RESET_PENDING))
		return;

	set_state_pe(port, PE_SNK_TRANSITION_TO_DEFAULT);
}

/**
 * PE_SNK_Transition_to_default
 */
static void pe_snk_transition_to_default_entry(int port)
{
	print_current_state(port);

	/* Reset flags */
	memset(&pe[port].flags_a, 0, sizeof(pe[port].flags_a));

	/* Reset DPM Request */
	pe[port].dpm_request = 0;

	/* Inform the TC Layer of Hard Reset */
	tc_hard_reset_request(port);
}

static void pe_snk_transition_to_default_run(int port)
{
	if (PE_CHK_FLAG(port, PE_FLAGS_PS_RESET_COMPLETE)) {
		PE_CLR_FLAG(port, PE_FLAGS_PS_RESET_COMPLETE);
		/* Inform the Protocol Layer that the Hard Reset is complete */
		prl_hard_reset_complete(port);
		set_state_pe(port, PE_SNK_STARTUP);
	}
}

/**
 * PE_SNK_Get_Source_Cap
 */
static void pe_snk_get_source_cap_entry(int port)
{
	print_current_state(port);

	/* Send a Get_Source_Cap Message */
	tx_emsg[port].len = 0;
	send_ctrl_msg(port, TCPCI_MSG_SOP, PD_CTRL_GET_SOURCE_CAP);
}

static void pe_snk_get_source_cap_run(int port)
{
	if (PE_CHK_FLAG(port, PE_FLAGS_TX_COMPLETE)) {
		PE_CLR_FLAG(port, PE_FLAGS_TX_COMPLETE);

		set_state_pe(port, PE_SNK_READY);
	}
}

/**
 * PE_SNK_Send_Soft_Reset and PE_SRC_Send_Soft_Reset
 */
static void pe_send_soft_reset_entry(int port)
{
	print_current_state(port);

	PE_CLR_FLAG(port, PE_FLAGS_ENTERING_EPR);
	PE_CLR_FLAG(port, PE_FLAGS_EPR_EXPLICIT_EXIT);

	/* Reset Protocol Layer (softly) */
	prl_reset_soft(port);

	pe_sender_response_msg_entry(port);

	/*
	 * Mark the temporary timer PE_TIMER_TIMEOUT as expired to limit
	 * to sending a single SoftReset message.
	 */
	pd_timer_enable(port, PE_TIMER_TIMEOUT, 0);
}

static void pe_send_soft_reset_run(int port)
{
	int type;
	int cnt;
	int ext;
	enum pe_msg_check msg_check;

	/* Wait until protocol layer is running */
	if (!prl_is_running(port))
		return;

	/*
	 * Protocol layer is running, so need to send a single SoftReset.
	 * Use temporary timer to act as a flag to keep this as a single
	 * message send.
	 */
	if (!pd_timer_is_disabled(port, PE_TIMER_TIMEOUT)) {
		pd_timer_disable(port, PE_TIMER_TIMEOUT);

		/*
		 * TODO(b/150614211): Soft reset type should match
		 * unexpected incoming message type
		 */
		/* Send Soft Reset message */
		send_ctrl_msg(port, pe[port].soft_reset_sop,
			      PD_CTRL_SOFT_RESET);

		return;
	}

	/*
	 * Check the state of the message sent
	 */
	msg_check = pe_sender_response_msg_run(port);

	/*
	 * Handle discarded message
	 */
	if (msg_check == PE_MSG_DISCARDED) {
		pe_set_ready_state(port);
		return;
	}

	/*
	 * Transition to the PE_SNK_Send_Capabilities or
	 * PE_SRC_Send_Capabilities state when:
	 *   1) An Accept Message has been received.
	 */
	if (msg_check == PE_MSG_SENT &&
	    PE_CHK_FLAG(port, PE_FLAGS_MSG_RECEIVED)) {
		PE_CLR_FLAG(port, PE_FLAGS_MSG_RECEIVED);

		type = PD_HEADER_TYPE(rx_emsg[port].header);
		cnt = PD_HEADER_CNT(rx_emsg[port].header);
		ext = PD_HEADER_EXT(rx_emsg[port].header);

		if ((ext == 0) && (cnt == 0) && (type == PD_CTRL_ACCEPT)) {
			if (pe[port].power_role == PD_ROLE_SINK)
				set_state_pe(port,
					     PE_SNK_WAIT_FOR_CAPABILITIES);
			else
				set_state_pe(port, PE_SRC_SEND_CAPABILITIES);
			return;
		}
	}

	/*
	 * Transition to PE_SNK_Hard_Reset or PE_SRC_Hard_Reset on Sender
	 * Response Timer Timeout or Protocol Layer or Protocol Error
	 */
	if (pd_timer_is_expired(port, PE_TIMER_SENDER_RESPONSE) ||
	    PE_CHK_FLAG(port, PE_FLAGS_PROTOCOL_ERROR)) {
		PE_CLR_FLAG(port, PE_FLAGS_PROTOCOL_ERROR);
		pe_set_hard_reset(port);
		return;
	}
}

static void pe_send_soft_reset_exit(int port)
{
	pe_sender_response_msg_exit(port);
	pd_timer_disable(port, PE_TIMER_TIMEOUT);
}

/**
 * PE_SNK_Soft_Reset and PE_SNK_Soft_Reset
 */
static void pe_soft_reset_entry(int port)
{
	print_current_state(port);

	send_ctrl_msg(port, TCPCI_MSG_SOP, PD_CTRL_ACCEPT);
}

static void pe_soft_reset_run(int port)
{
	if (PE_CHK_FLAG(port, PE_FLAGS_TX_COMPLETE)) {
		PE_CLR_FLAG(port, PE_FLAGS_TX_COMPLETE);

		if (pe[port].power_role == PD_ROLE_SINK)
			set_state_pe(port, PE_SNK_WAIT_FOR_CAPABILITIES);
		else
			set_state_pe(port, PE_SRC_SEND_CAPABILITIES);
	} else if (PE_CHK_FLAG(port, PE_FLAGS_PROTOCOL_ERROR)) {
		PE_CLR_FLAG(port, PE_FLAGS_PROTOCOL_ERROR);
		pe_set_hard_reset(port);
	}
}

/**
 * PE_SRC_Not_Supported and PE_SNK_Not_Supported
 *
 * 6.7.1 Soft Reset and Protocol Error (Revision 2.0, Version 1.3)
 * An unrecognized or unsupported Message (except for a Structured VDM),
 * received in the PE_SNK_Ready or PE_SRC_Ready states, Shall Not cause
 * a Soft_Reset Message to be generated but instead a Reject Message
 * Shall be generated.
 */
static void pe_send_not_supported_entry(int port)
{
	print_current_state(port);

	/* Request the Protocol Layer to send a Not_Supported Message. */
	if (prl_get_rev(port, TCPCI_MSG_SOP) > PD_REV20)
		send_ctrl_msg(port, TCPCI_MSG_SOP, PD_CTRL_NOT_SUPPORTED);
	else
		send_ctrl_msg(port, TCPCI_MSG_SOP, PD_CTRL_REJECT);
}

static void pe_send_not_supported_run(int port)
{
	if (PE_CHK_FLAG(port, PE_FLAGS_TX_COMPLETE)) {
		PE_CLR_FLAG(port, PE_FLAGS_TX_COMPLETE);
		pe_set_ready_state(port);
	}
}

/**
 * PE_SRC_Chunk_Received and PE_SNK_Chunk_Received
 *
 * 6.11.2.1.1 Architecture of Device Including Chunking Layer (Revision 3.0,
 * Version 2.0): If a PD Device or Cable Marker has no requirement to handle any
 * message requiring more than one Chunk of any Extended Message, it May omit
 * the Chunking Layer. In this case it Shall implement the
 * ChunkingNotSupportedTimer to ensure compatible operation with partners which
 * support Chunking.
 *
 * See also:
 * 6.6.18.1 ChunkingNotSupportedTimer
 * 8.3.3.6  Not Supported Message State Diagrams
 */
__maybe_unused static void pe_chunk_received_entry(int port)
{
	if (!1/*IS_ENABLED(CONFIG_USB_PD_REV30)*/ ||
	    0/*IS_ENABLED(CONFIG_USB_PD_EXTENDED_MESSAGES)*/)
		assert(0);

	print_current_state(port);
	pd_timer_enable(port, PE_TIMER_CHUNKING_NOT_SUPPORTED,
			PD_T_CHUNKING_NOT_SUPPORTED);
}

__maybe_unused static void pe_chunk_received_run(int port)
{
	if (!1/*IS_ENABLED(CONFIG_USB_PD_REV30)*/ ||
	    0/*IS_ENABLED(CONFIG_USB_PD_EXTENDED_MESSAGES)*/)
		assert(0);

	if (pd_timer_is_expired(port, PE_TIMER_CHUNKING_NOT_SUPPORTED))
		set_state_pe(port, PE_SEND_NOT_SUPPORTED);
}

__maybe_unused static void pe_chunk_received_exit(int port)
{
	pd_timer_disable(port, PE_TIMER_CHUNKING_NOT_SUPPORTED);
}

/**
 * PE_SRC_Ping
 */
static void pe_src_ping_entry(int port)
{
	print_current_state(port);
	send_ctrl_msg(port, TCPCI_MSG_SOP, PD_CTRL_PING);
}

static void pe_src_ping_run(int port)
{
	if (PE_CHK_FLAG(port, PE_FLAGS_TX_COMPLETE)) {
		PE_CLR_FLAG(port, PE_FLAGS_TX_COMPLETE);
		set_state_pe(port, PE_SRC_READY);
	}
}


/**
 * PE_DRS_Evaluate_Swap
 * PE_DRS_UFP_DFP_Evaluate_Swap and PE_DRS_DFP_UFP_Evaluate_Swap embedded here.
 */
static void pe_drs_evaluate_swap_entry(int port)
{
	print_current_state(port);

	/* Get evaluation of Data Role Swap request from DPM */
	if (pd_check_data_swap(port, pe[port].data_role)) {
		PE_SET_FLAG(port, PE_FLAGS_ACCEPT);
		/*
		 * PE_DRS_UFP_DFP_Accept_Swap and
		 * PE_DRS_DFP_UFP_Accept_Swap states embedded here.
		 */
		send_ctrl_msg(port, TCPCI_MSG_SOP, PD_CTRL_ACCEPT);
		/*
		 * The PD spec implies that the PE transitions through
		 * PE_DRS_*_Accept_Swap and PE_DRS_Change and updates the data
		 * role instantaneously, but this PE doesn't. During the
		 * transition, do not validate the data role of incoming
		 * messages, in case the port partner transitioned faster.
		 */
		prl_set_data_role_check(port, false);
	} else {
		/*
		 * PE_DRS_UFP_DFP_Reject_Swap and PE_DRS_DFP_UFP_Reject_Swap
		 * states embedded here.
		 */
		send_ctrl_msg(port, TCPCI_MSG_SOP, PD_CTRL_REJECT);
	}
}

static void pe_drs_evaluate_swap_run(int port)
{
	if (PE_CHK_FLAG(port, PE_FLAGS_TX_COMPLETE)) {
		PE_CLR_FLAG(port, PE_FLAGS_TX_COMPLETE);

		/* Accept Message sent. Transtion to PE_DRS_Change */
		if (PE_CHK_FLAG(port, PE_FLAGS_ACCEPT)) {
			PE_CLR_FLAG(port, PE_FLAGS_ACCEPT);
			set_state_pe(port, PE_DRS_CHANGE);
		} else {
			/*
			 * Message sent. Transition back to PE_SRC_Ready or
			 * PE_SNK_Ready.
			 */
			pe_set_ready_state(port);
		}
	}
}

/**
 * PE_DRS_Change
 */
static void pe_drs_change_entry(int port)
{
	print_current_state(port);

	/*
	 * PE_DRS_UFP_DFP_Change_to_DFP and PE_DRS_DFP_UFP_Change_to_UFP
	 * states embedded here.
	 */
	/* Request DPM to change port data role */
	pd_request_data_swap(port);
}

static void pe_drs_change_run(int port)
{
	/* Wait until the data role is changed */
	if (pe[port].data_role == pd_get_data_role(port))
		return;

	/* Update the data role */
	pe[port].data_role = pd_get_data_role(port);
	prl_set_data_role_check(port, true);

	if (pe[port].data_role == PD_ROLE_DFP)
		PE_CLR_FLAG(port, PE_FLAGS_DR_SWAP_TO_DFP);

	/*
	 * Port changed. Transition back to PE_SRC_Ready or
	 * PE_SNK_Ready.
	 */
	pe_set_ready_state(port);
}

/**
 * PE_DRS_Send_Swap
 */
static void pe_drs_send_swap_entry(int port)
{
	print_current_state(port);

	/*
	 * PE_DRS_UFP_DFP_Send_Swap and PE_DRS_DFP_UFP_Send_Swap
	 * states embedded here.
	 */
	/* Request the Protocol Layer to send a DR_Swap Message */
	send_ctrl_msg(port, TCPCI_MSG_SOP, PD_CTRL_DR_SWAP);
	pe_sender_response_msg_entry(port);
}

static void pe_drs_send_swap_run(int port)
{
	int type;
	int cnt;
	int ext;
	enum pe_msg_check msg_check;

	/*
	 * Check the state of the message sent
	 */
	msg_check = pe_sender_response_msg_run(port);

	/*
	 * Transition to PE_DRS_Change when:
	 *   1) An Accept Message is received.
	 *
	 * Transition to PE_SRC_Ready or PE_SNK_Ready state when:
	 *   1) A Reject Message is received.
	 *   2) Or a Wait Message is received.
	 */
	if ((msg_check & PE_MSG_SENT) &&
	    PE_CHK_FLAG(port, PE_FLAGS_MSG_RECEIVED)) {
		PE_CLR_FLAG(port, PE_FLAGS_MSG_RECEIVED);

		type = PD_HEADER_TYPE(rx_emsg[port].header);
		cnt = PD_HEADER_CNT(rx_emsg[port].header);
		ext = PD_HEADER_EXT(rx_emsg[port].header);

		if ((ext == 0) && (cnt == 0)) {
			if (type == PD_CTRL_ACCEPT) {
				set_state_pe(port, PE_DRS_CHANGE);
				return;
			} else if ((type == PD_CTRL_REJECT) ||
				   (type == PD_CTRL_WAIT) ||
				   (type == PD_CTRL_NOT_SUPPORTED)) {
				pe_set_ready_state(port);
				return;
			}
		}
	}

	/*
	 * Transition to PE_SRC_Ready or PE_SNK_Ready state when:
	 *   1) the SenderResponseTimer times out.
	 *   2) Message was discarded.
	 */
	if ((msg_check & PE_MSG_DISCARDED) ||
	    pd_timer_is_expired(port, PE_TIMER_SENDER_RESPONSE))
		pe_set_ready_state(port);
}

static void pe_drs_send_swap_exit(int port)
{
	pe_sender_response_msg_exit(port);
}

/**
 * PE_PRS_SRC_SNK_Evaluate_Swap
 */
static void pe_prs_src_snk_evaluate_swap_entry(int port)
{
	print_current_state(port);

	if (!pd_check_power_swap(port)) {
		/* PE_PRS_SRC_SNK_Reject_PR_Swap state embedded here */
		send_ctrl_msg(port, TCPCI_MSG_SOP, PD_CTRL_REJECT);
	} else {
		tc_request_power_swap(port);
		/* PE_PRS_SRC_SNK_Accept_Swap state embedded here */
		PE_SET_FLAG(port, PE_FLAGS_ACCEPT);
		send_ctrl_msg(port, TCPCI_MSG_SOP, PD_CTRL_ACCEPT);
	}
}

static void pe_prs_src_snk_evaluate_swap_run(int port)
{
	if (PE_CHK_FLAG(port, PE_FLAGS_TX_COMPLETE)) {
		PE_CLR_FLAG(port, PE_FLAGS_TX_COMPLETE);

		if (PE_CHK_FLAG(port, PE_FLAGS_ACCEPT)) {
			PE_CLR_FLAG(port, PE_FLAGS_ACCEPT);

			/*
			 * Clear any pending DPM power role swap request so we
			 * don't trigger a power role swap request back to src
			 * power role.
			 */
			PE_CLR_DPM_REQUEST(port, DPM_REQUEST_PR_SWAP);
			/*
			 * Power Role Swap OK, transition to
			 * PE_PRS_SRC_SNK_Transition_to_off
			 */
			set_state_pe(port, PE_PRS_SRC_SNK_TRANSITION_TO_OFF);
		} else {
			/* Message sent, return to PE_SRC_Ready */
			set_state_pe(port, PE_SRC_READY);
		}
	}
}

/**
 * PE_PRS_SRC_SNK_Transition_To_Off
 */
static void pe_prs_src_snk_transition_to_off_entry(int port)
{
	print_current_state(port);

	/* Contract is invalid */
	pe_invalidate_explicit_contract(port);

	pd_timer_enable(port, PE_TIMER_SRC_TRANSITION, PD_T_SRC_TRANSITION);
}

static void pe_prs_src_snk_transition_to_off_run(int port)
{
	/*
	 * This is a non-interruptible AMS and power is transitioning - hard
	 * reset on interruption.
	 */
	if (PE_CHK_FLAG(port, PE_FLAGS_MSG_RECEIVED)) {
		PE_CLR_FLAG(port, PE_FLAGS_MSG_RECEIVED);

		tc_pr_swap_complete(port, 0);
		pe_set_hard_reset(port);
		return;
	}

	/* Wait tSrcTransition (~ 25ms) before turning off VBUS */
	if (!pd_timer_is_expired(port, PE_TIMER_SRC_TRANSITION))
		return;

	if (!PE_CHK_FLAG(port, PE_FLAGS_SRC_SNK_SETTLE)) {
		PE_SET_FLAG(port, PE_FLAGS_SRC_SNK_SETTLE);
		/* Tell TypeC to power off the source */
		tc_src_power_off(port);

		pd_timer_enable(port, PE_TIMER_PS_SOURCE,
				PD_POWER_SUPPLY_TURN_OFF_DELAY);
		return;
	}

	/* Give time for supply to power off */
	if (pd_timer_is_expired(port, PE_TIMER_PS_SOURCE) &&
	    pd_check_vbus_level(port, VBUS_SAFE0V))
		set_state_pe(port, PE_PRS_SRC_SNK_ASSERT_RD);
}

static void pe_prs_src_snk_transition_to_off_exit(int port)
{
	PE_CLR_FLAG(port, PE_FLAGS_SRC_SNK_SETTLE);
	pd_timer_disable(port, PE_TIMER_SRC_TRANSITION);
	pd_timer_disable(port, PE_TIMER_PS_SOURCE);
}

/**
 * PE_PRS_SRC_SNK_Assert_Rd
 */
static void pe_prs_src_snk_assert_rd_entry(int port)
{
	print_current_state(port);

	/* Tell TypeC to swap from Attached.SRC to Attached.SNK */
	tc_prs_src_snk_assert_rd(port);
}

static void pe_prs_src_snk_assert_rd_run(int port)
{
	/* Wait until Rd is asserted */
	if (tc_is_attached_snk(port))
		set_state_pe(port, PE_PRS_SRC_SNK_WAIT_SOURCE_ON);
}

/**
 * PE_PRS_SRC_SNK_Wait_Source_On
 */
static void pe_prs_src_snk_wait_source_on_entry(int port)
{
	print_current_state(port);
	send_ctrl_msg(port, TCPCI_MSG_SOP, PD_CTRL_PS_RDY);
}

static void pe_prs_src_snk_wait_source_on_run(int port)
{
	if (pd_timer_is_disabled(port, PE_TIMER_PS_SOURCE) &&
	    PE_CHK_FLAG(port, PE_FLAGS_TX_COMPLETE)) {
		PE_CLR_FLAG(port, PE_FLAGS_TX_COMPLETE);

		/* Update pe power role */
		pe[port].power_role = pd_get_power_role(port);
		pd_timer_enable(port, PE_TIMER_PS_SOURCE, PD_T_PS_SOURCE_ON);
	}

	/*
	 * Transition to PE_SNK_Startup when:
	 *   1) A PS_RDY Message is received.
	 */
	if (!pd_timer_is_disabled(port, PE_TIMER_PS_SOURCE) &&
	    PE_CHK_FLAG(port, PE_FLAGS_MSG_RECEIVED)) {
		int type = PD_HEADER_TYPE(rx_emsg[port].header);
		int cnt = PD_HEADER_CNT(rx_emsg[port].header);
		int ext = PD_HEADER_EXT(rx_emsg[port].header);

		PE_CLR_FLAG(port, PE_FLAGS_MSG_RECEIVED);

		if ((ext == 0) && (cnt == 0) && (type == PD_CTRL_PS_RDY)) {
			PE_SET_FLAG(port, PE_FLAGS_PR_SWAP_COMPLETE);
			set_state_pe(port, PE_SNK_STARTUP);
		} else {
			int sop = PD_HEADER_GET_SOP(rx_emsg[port].header);
			/*
			 * USB PD 3.0 6.8.1:
			 * Receiving an unexpected message shall be responded
			 * to with a soft reset message.
			 */
			pe_send_soft_reset(port, sop);
		}
		return;
	}

	/*
	 * Transition to ErrorRecovery state when:
	 *   1) The PSSourceOnTimer times out.
	 *   2) PS_RDY not sent after retries.
	 */
	if (pd_timer_is_expired(port, PE_TIMER_PS_SOURCE) ||
	    PE_CHK_FLAG(port, PE_FLAGS_PROTOCOL_ERROR)) {
		PE_CLR_FLAG(port, PE_FLAGS_PROTOCOL_ERROR);

		set_state_pe(port, PE_WAIT_FOR_ERROR_RECOVERY);
		return;
	}
}

static void pe_prs_src_snk_wait_source_on_exit(int port)
{
	pd_timer_disable(port, PE_TIMER_PS_SOURCE);
	tc_pr_swap_complete(port, PE_CHK_FLAG(port, PE_FLAGS_PR_SWAP_COMPLETE));
}

/**
 * PE_PRS_SRC_SNK_Send_Swap
 */
static void pe_prs_src_snk_send_swap_entry(int port)
{
	print_current_state(port);

	/* Making an attempt to PR_Swap, clear we were possibly waiting */
	pd_timer_disable(port, PE_TIMER_PR_SWAP_WAIT);

	/* Request the Protocol Layer to send a PR_Swap Message. */
	send_ctrl_msg(port, TCPCI_MSG_SOP, PD_CTRL_PR_SWAP);
	pe_sender_response_msg_entry(port);
}

static void pe_prs_src_snk_send_swap_run(int port)
{
	int type;
	int cnt;
	int ext;
	enum pe_msg_check msg_check;

	/*
	 * Check the state of the message sent
	 */
	msg_check = pe_sender_response_msg_run(port);

	/*
	 * Transition to PE_PRS_SRC_SNK_Transition_To_Off when:
	 *   1) An Accept Message is received.
	 *
	 * Transition to PE_SRC_Ready state when:
	 *   1) A Reject Message is received.
	 *   2) Or a Wait Message is received.
	 */
	if ((msg_check & PE_MSG_SENT) &&
	    PE_CHK_FLAG(port, PE_FLAGS_MSG_RECEIVED)) {
		PE_CLR_FLAG(port, PE_FLAGS_MSG_RECEIVED);

		type = PD_HEADER_TYPE(rx_emsg[port].header);
		cnt = PD_HEADER_CNT(rx_emsg[port].header);
		ext = PD_HEADER_EXT(rx_emsg[port].header);

		if ((ext == 0) && (cnt == 0)) {
			if (type == PD_CTRL_ACCEPT) {
				pe[port].src_snk_pr_swap_counter = 0;
				tc_request_power_swap(port);
				set_state_pe(port,
					     PE_PRS_SRC_SNK_TRANSITION_TO_OFF);
			} else if (type == PD_CTRL_REJECT) {
				pe[port].src_snk_pr_swap_counter = 0;
				set_state_pe(port, PE_SRC_READY);
			} else if (type == PD_CTRL_WAIT) {
				if (pe[port].src_snk_pr_swap_counter <
				    N_SNK_SRC_PR_SWAP_COUNT) {
					PE_SET_FLAG(port,
						    PE_FLAGS_WAITING_PR_SWAP);
					pd_timer_enable(port,
							PE_TIMER_PR_SWAP_WAIT,
							PD_T_PR_SWAP_WAIT);
				}
				pe[port].src_snk_pr_swap_counter++;
				set_state_pe(port, PE_SRC_READY);
			}
			return;
		}
	}

	/*
	 * Transition to PE_SRC_Ready state when:
	 *   1) Or the SenderResponseTimer times out.
	 *   2) Message was discarded.
	 */
	if ((msg_check & PE_MSG_DISCARDED) ||
	    pd_timer_is_expired(port, PE_TIMER_SENDER_RESPONSE))
		set_state_pe(port, PE_SRC_READY);
}

static void pe_prs_src_snk_send_swap_exit(int port)
{
	pe_sender_response_msg_exit(port);
}

/**
 * PE_PRS_SNK_SRC_Evaluate_Swap
 */
static void pe_prs_snk_src_evaluate_swap_entry(int port)
{
	print_current_state(port);

	/*
	 * Cancel any pending PR swap request due to a received Wait since the
	 * partner just sent us a PR swap message.
	 */
	PE_CLR_FLAG(port, PE_FLAGS_WAITING_PR_SWAP);
	pe[port].src_snk_pr_swap_counter = 0;

	if (!pd_check_power_swap(port)) {
		/* PE_PRS_SNK_SRC_Reject_Swap state embedded here */
		send_ctrl_msg(port, TCPCI_MSG_SOP, PD_CTRL_REJECT);
	} else {
		tc_request_power_swap(port);
		/* PE_PRS_SNK_SRC_Accept_Swap state embedded here */
		PE_SET_FLAG(port, PE_FLAGS_ACCEPT);
		send_ctrl_msg(port, TCPCI_MSG_SOP, PD_CTRL_ACCEPT);
	}
}

static void pe_prs_snk_src_evaluate_swap_run(int port)
{
	if (PE_CHK_FLAG(port, PE_FLAGS_TX_COMPLETE)) {
		PE_CLR_FLAG(port, PE_FLAGS_TX_COMPLETE);
		if (PE_CHK_FLAG(port, PE_FLAGS_ACCEPT)) {
			PE_CLR_FLAG(port, PE_FLAGS_ACCEPT);

			/*
			 * Clear any pending DPM power role swap request so we
			 * don't trigger a power role swap request back to sink
			 * power role.
			 */
			PE_CLR_DPM_REQUEST(port, DPM_REQUEST_PR_SWAP);
			/*
			 * Accept message sent, transition to
			 * PE_PRS_SNK_SRC_Transition_to_off
			 */
			set_state_pe(port, PE_PRS_SNK_SRC_TRANSITION_TO_OFF);
		} else {
			/* Message sent, return to PE_SNK_Ready */
			set_state_pe(port, PE_SNK_READY);
		}
	}

	if (PE_CHK_FLAG(port, PE_FLAGS_PROTOCOL_ERROR)) {
		PE_CLR_FLAG(port, PE_FLAGS_PROTOCOL_ERROR);
		/*
		 * Protocol Error occurs while PR swap, this may
		 * brown out if the port-parnter can't hold VBUS
		 * for tSrcTransition. Notify TC that we end the PR
		 * swap and start to watch VBUS.
		 *
		 * TODO(b:155181980): issue soft reset on protocol error.
		 */
		tc_pr_swap_complete(port, 0);
	}
}

/**
 * PE_PRS_SNK_SRC_Transition_To_Off
 * PE_FRS_SNK_SRC_Transition_To_Off
 *
 * NOTE: Shared action code used for Power Role Swap and Fast Role Swap
 */
static void pe_prs_snk_src_transition_to_off_entry(int port)
{
	print_current_state(port);

	if (!1/*IS_ENABLED(CONFIG_USB_PD_REV30)*/ || !pe_in_frs_mode(port))
		tc_snk_power_off(port);

	pd_timer_enable(port, PE_TIMER_PS_SOURCE, PD_T_PS_SOURCE_OFF);
}

static void pe_prs_snk_src_transition_to_off_run(int port)
{
	int type;
	int cnt;
	int ext;

	/*
	 * Transition to ErrorRecovery state when:
	 *   1) The PSSourceOffTimer times out.
	 */
	if (pd_timer_is_expired(port, PE_TIMER_PS_SOURCE))
		set_state_pe(port, PE_WAIT_FOR_ERROR_RECOVERY);

	/*
	 * Transition to PE_PRS_SNK_SRC_Assert_Rp when:
	 *   1) An PS_RDY Message is received.
	 */
	else if (PE_CHK_FLAG(port, PE_FLAGS_MSG_RECEIVED)) {
		PE_CLR_FLAG(port, PE_FLAGS_MSG_RECEIVED);

		type = PD_HEADER_TYPE(rx_emsg[port].header);
		cnt = PD_HEADER_CNT(rx_emsg[port].header);
		ext = PD_HEADER_EXT(rx_emsg[port].header);

		if ((ext == 0) && (cnt == 0) && (type == PD_CTRL_PS_RDY)) {
			/*
			 * FRS: We are always ready to drive vSafe5v, so just
			 * skip PE_FRS_SNK_SRC_Vbus_Applied and go direct to
			 * PE_FRS_SNK_SRC_Assert_Rp
			 */
			set_state_pe(port, PE_PRS_SNK_SRC_ASSERT_RP);
		}
	}
}

static void pe_prs_snk_src_transition_to_off_exit(int port)
{
	pd_timer_disable(port, PE_TIMER_PS_SOURCE);
}

/**
 * PE_PRS_SNK_SRC_Assert_Rp
 * PE_FRS_SNK_SRC_Assert_Rp
 *
 * NOTE: Shared action code used for Power Role Swap and Fast Role Swap
 */
static void pe_prs_snk_src_assert_rp_entry(int port)
{
	print_current_state(port);

	/*
	 * Tell TypeC to Power/Fast Role Swap (PRS/FRS) from
	 * Attached.SNK to Attached.SRC
	 */
	tc_prs_snk_src_assert_rp(port);
}

static void pe_prs_snk_src_assert_rp_run(int port)
{
	/* Wait until TypeC is in the Attached.SRC state */
	if (tc_is_attached_src(port)) {
		if (!1/*IS_ENABLED(CONFIG_USB_PD_REV30)*/ || !pe_in_frs_mode(port)) {
			/* Contract is invalid now */
			pe_invalidate_explicit_contract(port);
		}
		set_state_pe(port, PE_PRS_SNK_SRC_SOURCE_ON);
	}
}

/**
 * PE_PRS_SNK_SRC_Source_On
 * PE_FRS_SNK_SRC_Source_On
 *
 * NOTE: Shared action code used for Power Role Swap and Fast Role Swap
 */
static void pe_prs_snk_src_source_on_entry(int port)
{
	print_current_state(port);

	/*
	 * VBUS was enabled when the TypeC state machine entered
	 * Attached.SRC state
	 */
	pd_timer_enable(port, PE_TIMER_PS_SOURCE,
			PD_POWER_SUPPLY_TURN_ON_DELAY);
}

static void pe_prs_snk_src_source_on_run(int port)
{
	/* Wait until power supply turns on */
	if (!pd_timer_is_disabled(port, PE_TIMER_PS_SOURCE)) {
		if (!pd_timer_is_expired(port, PE_TIMER_PS_SOURCE))
			return;

		/* update pe power role */
		pe[port].power_role = pd_get_power_role(port);
		send_ctrl_msg(port, TCPCI_MSG_SOP, PD_CTRL_PS_RDY);
		/* reset timer so PD_CTRL_PS_RDY isn't sent again */
		pd_timer_disable(port, PE_TIMER_PS_SOURCE);
	}

	/*
	 * Transition to ErrorRecovery state when:
	 *   1) On protocol error
	 */
	else if (PE_CHK_FLAG(port, PE_FLAGS_PROTOCOL_ERROR)) {
		PE_CLR_FLAG(port, PE_FLAGS_PROTOCOL_ERROR);
		set_state_pe(port, PE_WAIT_FOR_ERROR_RECOVERY);
	}

	else if (PE_CHK_FLAG(port, PE_FLAGS_TX_COMPLETE)) {
		PE_CLR_FLAG(port, PE_FLAGS_TX_COMPLETE);

		/* Run swap source timer on entry to pe_src_startup */
		PE_SET_FLAG(port, PE_FLAGS_PR_SWAP_COMPLETE);
		set_state_pe(port, PE_SRC_STARTUP);
	}
}

static void pe_prs_snk_src_source_on_exit(int port)
{
	pd_timer_disable(port, PE_TIMER_PS_SOURCE);
	tc_pr_swap_complete(port, PE_CHK_FLAG(port, PE_FLAGS_PR_SWAP_COMPLETE));
}

/**
 * PE_PRS_SNK_SRC_Send_Swap
 * PE_FRS_SNK_SRC_Send_Swap
 *
 * NOTE: Shared action code used for Power Role Swap and Fast Role Swap
 */
static void pe_prs_snk_src_send_swap_entry(int port)
{
	print_current_state(port);

	/*
	 * PRS_SNK_SRC_SEND_SWAP
	 *     Request the Protocol Layer to send a PR_Swap Message.
	 *
	 * FRS_SNK_SRC_SEND_SWAP
	 *     Hardware should have turned off sink power and started
	 *     bringing Vbus to vSafe5.
	 *     Request the Protocol Layer to send a FR_Swap Message.
	 */
	if (1/*IS_ENABLED(CONFIG_USB_PD_REV30)*/) {
		send_ctrl_msg(port, TCPCI_MSG_SOP,
			      pe_in_frs_mode(port) ? PD_CTRL_FR_SWAP :
						     PD_CTRL_PR_SWAP);
	} else {
		send_ctrl_msg(port, TCPCI_MSG_SOP, PD_CTRL_PR_SWAP);
	}
	pe_sender_response_msg_entry(port);
}

static void pe_prs_snk_src_send_swap_run(int port)
{
	int type;
	int cnt;
	int ext;
	enum pe_msg_check msg_check;

	/*
	 * Check the state of the message sent
	 */
	msg_check = pe_sender_response_msg_run(port);

	/*
	 * Handle discarded message
	 */
	if (msg_check & PE_MSG_DISCARDED) {
		set_state_pe(port, pe_in_frs_mode(port) ?
					   PE_WAIT_FOR_ERROR_RECOVERY :
					   PE_SNK_READY);
		return;
	}

	/*
	 * Transition to PE_PRS_SNK_SRC_Transition_to_off when:
	 *   1) An Accept Message is received.
	 *
	 * PRS: Transition to PE_SNK_Ready state when:
	 * FRS: Transition to ErrorRecovery state when:
	 *   1) A Reject Message is received.
	 *   2) Or a Wait Message is received.
	 */
	if ((msg_check & PE_MSG_SENT) &&
	    PE_CHK_FLAG(port, PE_FLAGS_MSG_RECEIVED)) {
		PE_CLR_FLAG(port, PE_FLAGS_MSG_RECEIVED);

		type = PD_HEADER_TYPE(rx_emsg[port].header);
		cnt = PD_HEADER_CNT(rx_emsg[port].header);
		ext = PD_HEADER_EXT(rx_emsg[port].header);

		if ((ext == 0) && (cnt == 0)) {
			if (type == PD_CTRL_ACCEPT) {
				tc_request_power_swap(port);
				set_state_pe(port,
					     PE_PRS_SNK_SRC_TRANSITION_TO_OFF);
			} else if ((type == PD_CTRL_REJECT) ||
				   (type == PD_CTRL_WAIT)) {
				if (1/*IS_ENABLED(CONFIG_USB_PD_REV30)*/)
					set_state_pe(
						port,
						pe_in_frs_mode(port) ?
							PE_WAIT_FOR_ERROR_RECOVERY :
							PE_SNK_READY);
				else
					set_state_pe(port, PE_SNK_READY);
			}
			return;
		}
	}

	/*
	 * PRS: Transition to PE_SNK_Ready state when:
	 * FRS: Transition to ErrorRecovery state when:
	 *   1) The SenderResponseTimer times out.
	 */
	if (pd_timer_is_expired(port, PE_TIMER_SENDER_RESPONSE)) {
		if (1/*IS_ENABLED(CONFIG_USB_PD_REV30)*/)
			set_state_pe(port, pe_in_frs_mode(port) ?
						   PE_WAIT_FOR_ERROR_RECOVERY :
						   PE_SNK_READY);
		else
			set_state_pe(port, PE_SNK_READY);
		return;
	}
	/*
	 * FRS Only: Transition to ErrorRecovery state when:
	 *   2) The FR_Swap Message is not sent after retries (a GoodCRC Message
	 *      has not been received). A soft reset Shall Not be initiated in
	 *      this case.
	 */
	if (1/*IS_ENABLED(CONFIG_USB_PD_REV30)*/ && pe_in_frs_mode(port) &&
	    PE_CHK_FLAG(port, PE_FLAGS_PROTOCOL_ERROR)) {
		PE_CLR_FLAG(port, PE_FLAGS_PROTOCOL_ERROR);
		set_state_pe(port, PE_WAIT_FOR_ERROR_RECOVERY);
	}
}

static void pe_prs_snk_src_send_swap_exit(int port)
{
	pe_sender_response_msg_exit(port);
}

/**
 * PE_FRS_SNK_SRC_Start_AMS
 */
__maybe_unused static void pe_frs_snk_src_start_ams_entry(int port)
{
	if (!1/*IS_ENABLED(CONFIG_USB_PD_REV30)*/)
		assert(0);

	print_current_state(port);

	/* Inform Protocol Layer this is start of AMS */
	PE_SET_FLAG(port, PE_FLAGS_LOCALLY_INITIATED_AMS);

	/* Shared PRS/FRS code, indicate FRS path */
	PE_SET_FLAG(port, PE_FLAGS_FAST_ROLE_SWAP_PATH);

	/*
	 * Invalidate the contract after the FRS flags set so the
	 * flags can be propagated to this function.
	 */
	if (port_frs_disable_until_source_on(port)) {
		/*
		 * Delay disable FRS until starting sourcing VBUS.
		 * Some boards need to extend the FRS enablement until the
		 * vSafe5V hitted (rather than FRS Rx received) then it can turn
		 * the source on automatically.
		 */
		pe_invalidate_explicit_contract_frs_untouched(port);
	} else {
		pe_invalidate_explicit_contract(port);
	}

	set_state_pe(port, PE_PRS_SNK_SRC_SEND_SWAP);
}

/**
 * PE_PRS_FRS_SHARED
 */
__maybe_unused static void pe_prs_frs_shared_entry(int port)
{
	if (!1/*IS_ENABLED(CONFIG_USB_PD_REV30)*/)
		assert(0);

	/*
	 * Shared PRS/FRS code, assume PRS path
	 *
	 * This is the super state entry. It will be called before
	 * the first entry state to get into the PRS/FRS path.
	 * For FRS, PE_FRS_SNK_SRC_START_AMS entry will be called
	 * after this and that will set for the FRS path.
	 */
	PE_CLR_FLAG(port, PE_FLAGS_FAST_ROLE_SWAP_PATH);
}

__maybe_unused static void pe_prs_frs_shared_exit(int port)
{
	if (!1/*IS_ENABLED(CONFIG_USB_PD_REV30)*/)
		assert(0);

	/*
	 * Shared PRS/FRS code, when not in shared path
	 * indicate PRS path
	 */
	PE_CLR_FLAG(port, PE_FLAGS_FAST_ROLE_SWAP_PATH);
}

/**
 * PE_BIST_TX
 */
static void pe_bist_tx_entry(int port)
{
	uint32_t *payload = (uint32_t *)rx_emsg[port].buf;
	uint8_t mode = BIST_MODE(payload[0]);
	int vbus_mv;
	int ibus_ma;

	print_current_state(port);

	/* Get the current nominal VBUS value */
	if (pe[port].power_role == PD_ROLE_SOURCE) {
		const uint32_t *src_pdo;
		uint32_t unused;

		dpm_get_source_pdo(&src_pdo, port);
		pd_extract_pdo_power(src_pdo[pe[port].requested_idx - 1],
				     &ibus_ma, &vbus_mv, &unused);
	} else {
		vbus_mv = pe[port].supply_voltage;
	}

	/* If VBUS is not at vSafe5V, then don't enter BIST test mode */
	if (vbus_mv != PD_V_SAFE5V_NOM) {
		pe_set_ready_state(port);
		return;
	}

	if (mode == BIST_CARRIER_MODE_2) {
		/*
		 * PE_BIST_Carrier_Mode embedded here.
		 * See PD 3.0 section 6.4.3.1 BIST Carrier Mode 2: With a BIST
		 * Carrier Mode 2 BIST Data Object, the UUT Shall send out a
		 * continuous string of BMC-encoded alternating "1"s and “0”s.
		 * The UUT Shall exit the Continuous BIST Mode within
		 * tBISTContMode of this Continuous BIST Mode being enabled.
		 */
		send_ctrl_msg(port, TCPCI_MSG_TX_BIST_MODE_2, 0);
		pd_timer_enable(port, PE_TIMER_BIST_CONT_MODE,
				PD_T_BIST_CONT_MODE);
	} else if (mode == BIST_TEST_DATA) {
		/*
		 * See PD 3.0 section 6.4.3.2 BIST Test Data:
		 * With a BIST Test Data BIST Data Object, the UUT Shall return
		 * a GoodCRC Message and Shall enter a test mode in which it
		 * sends no further Messages except for GoodCRC Messages in
		 * response to received Messages.... The test Shall be ended by
		 * sending Hard Reset Signaling to reset the UUT.
		 */
		if (tcpc_set_bist_test_mode(port, true) != EC_SUCCESS)
			CPRINTS("C%d: Failed to enter BIST Test Mode", port);
	} else if (1/*IS_ENABLED(CONFIG_USB_PD_REV30)*/ &&
		   mode == BIST_SHARED_MODE_ENTER) {
		/* Notify the DPM and return to ready */
		dpm_bist_shared_mode_enter(port);
		pe_set_ready_state(port);
		return;
	} else if (1/*IS_ENABLED(CONFIG_USB_PD_REV30)*/ &&
		   mode == BIST_SHARED_MODE_EXIT) {
		/* Notify the DPM and return to ready */
		dpm_bist_shared_mode_exit(port);
		pe_set_ready_state(port);
		return;
	} else {
		/* Ignore unsupported BIST messages. */
		pe_set_ready_state(port);
		return;
	}
}

static void pe_bist_tx_run(int port)
{
	if (pd_timer_is_expired(port, PE_TIMER_BIST_CONT_MODE)) {
		/*
		 * Entry point to disable BIST in TCPC if that's not already
		 * handled automatically by the TCPC. Unless this method is
		 * implemented in a TCPM driver, this function does nothing.
		 */
		tcpm_reset_bist_type_2(port);

		if (pe[port].power_role == PD_ROLE_SOURCE)
			set_state_pe(port, PE_SRC_TRANSITION_TO_DEFAULT);
		else
			set_state_pe(port, PE_SNK_TRANSITION_TO_DEFAULT);
	} else {
		/*
		 * We are in test data mode and no further Messages except for
		 * GoodCRC Messages in response to received Messages will
		 * be sent.
		 */
		if (PE_CHK_FLAG(port, PE_FLAGS_MSG_RECEIVED))
			PE_CLR_FLAG(port, PE_FLAGS_MSG_RECEIVED);
	}
}

static void pe_bist_tx_exit(int port)
{
	pd_timer_disable(port, PE_TIMER_BIST_CONT_MODE);
}

/**
 * Give_Sink_Cap Message
 */
static void pe_snk_give_sink_cap_entry(int port)
{
	print_current_state(port);

	/* Send a Sink_Capabilities Message */
	tx_emsg[port].len = pd_snk_pdo_cnt * 4;
	memcpy(tx_emsg[port].buf, (uint8_t *)pd_snk_pdo, tx_emsg[port].len);
	send_data_msg(port, TCPCI_MSG_SOP, PD_DATA_SINK_CAP);
}

static void pe_snk_give_sink_cap_run(int port)
{
	if (PE_CHK_FLAG(port, PE_FLAGS_TX_COMPLETE)) {
		PE_CLR_FLAG(port, PE_FLAGS_TX_COMPLETE);
		pe_set_ready_state(port);
		return;
	}

	if (pe_check_outgoing_discard(port))
		return;
}

/**
 * Wait For Error Recovery
 */
static void pe_wait_for_error_recovery_entry(int port)
{
	print_current_state(port);
	tc_start_error_recovery(port);
}

static void pe_wait_for_error_recovery_run(int port)
{
	/* Stay here until error recovery is complete */
}

uint32_t pd_compose_svdm_req_header(int port, enum tcpci_msg_type type,
				    uint16_t svid, int cmd)
{
	return VDO(svid, 1,
		   VDO_SVDM_VERS_MAJOR(pd_get_vdo_ver(port, pe[port].tx_type)) |
			   VDM_VERS_MINOR | cmd);
}

/**
 * PE_DEU_SEND_ENTER_USB
 */
static void pe_enter_usb_entry(int port)
{
	uint32_t usb4_payload;

	print_current_state(port);

	if (!0/*IS_ENABLED(CONFIG_USB_PD_USB4)*/) {
		pe_set_ready_state(port);
		return;
	}

	/* Port is already in USB4 mode, do not send enter USB message again */
	if (enter_usb_entry_is_done(port)) {
		pe_set_ready_state(port);
		return;
	}

	if ((pe[port].tx_type == TCPCI_MSG_SOP_PRIME ||
	     pe[port].tx_type == TCPCI_MSG_SOP_PRIME_PRIME) &&
	    !tc_is_vconn_src(port)) {
		if (port_try_vconn_swap_on(port))
			return;
	}

	pe[port].tx_type = TCPCI_MSG_SOP;
	usb4_payload = enter_usb_setup_next_msg(port, &pe[port].tx_type);

	if (!usb4_payload) {
		enter_usb_failed(port);
		pe_set_ready_state(port);
		return;
	}

	tx_emsg[port].len = sizeof(usb4_payload);

	memcpy(tx_emsg[port].buf, &usb4_payload, tx_emsg[port].len);
	send_data_msg(port, pe[port].tx_type, PD_DATA_ENTER_USB);
	pe_sender_response_msg_entry(port);
}

static void pe_enter_usb_run(int port)
{
	enum pe_msg_check msg_check;

	if (!0/*IS_ENABLED(CONFIG_USB_PD_USB4)*/) {
		pe_set_ready_state(port);
		return;
	}

	/*
	 * Check the state of the message sent
	 */
	msg_check = pe_sender_response_msg_run(port);

	/*
	 * Handle Discarded message, return to PE_SNK/SRC_READY
	 */
	if (msg_check & PE_MSG_DISCARDED) {
		pe_set_ready_state(port);
		return;
	} else if (msg_check == PE_MSG_SEND_PENDING) {
		/* Wait until message is sent */
		return;
	}

	if (pd_timer_is_expired(port, PE_TIMER_SENDER_RESPONSE)) {
		pe_set_ready_state(port);
		enter_usb_failed(port);
		return;
	}

	if (PE_CHK_FLAG(port, PE_FLAGS_MSG_RECEIVED)) {
		int cnt = PD_HEADER_CNT(rx_emsg[port].header);
		int type = PD_HEADER_TYPE(rx_emsg[port].header);
		int sop = PD_HEADER_GET_SOP(rx_emsg[port].header);

		PE_CLR_FLAG(port, PE_FLAGS_MSG_RECEIVED);

		/* Only look at control messages */
		if (cnt == 0) {
			/* Accept message received */
			if (type == PD_CTRL_ACCEPT) {
				enter_usb_accepted(port, sop);
			} else if (type == PD_CTRL_REJECT) {
				enter_usb_rejected(port, sop);
			} else {
				/*
				 * Unexpected control message received.
				 * Send Soft Reset.
				 */
				pe_send_soft_reset(port, sop);
				return;
			}
		} else {
			/* Unexpected data message received. Send Soft reset */
			pe_send_soft_reset(port, sop);
			return;
		}
		pe_set_ready_state(port);
	}
}

static void pe_enter_usb_exit(int port)
{
	pe_sender_response_msg_exit(port);
}


/*
 * PE_DR_SNK_Get_Sink_Cap and PE_SRC_Get_Sink_Cap State (shared)
 */
static void pe_dr_get_sink_cap_entry(int port)
{
	print_current_state(port);

	/* Send a Get Sink Cap Message */
	send_ctrl_msg(port, TCPCI_MSG_SOP, PD_CTRL_GET_SINK_CAP);
	pe_sender_response_msg_entry(port);
}

static void pe_dr_get_sink_cap_run(int port)
{
	int type;
	int cnt;
	int ext;
	enum pe_msg_check msg_check;
	enum tcpci_msg_type sop;

	/*
	 * Check the state of the message sent
	 */
	msg_check = pe_sender_response_msg_run(port);

	/*
	 * Transition to PE_[SRC,SNK]_Ready when:
	 *   1) A Sink_Capabilities Message is received
	 *   2) Or SenderResponseTimer times out
	 *   3) Or a Reject Message is received.
	 *
	 * Transition to PE_SEND_SOFT_RESET state when:
	 *   1) An unexpected message is received
	 */
	if ((msg_check & PE_MSG_SENT) &&
	    PE_CHK_FLAG(port, PE_FLAGS_MSG_RECEIVED)) {
		PE_CLR_FLAG(port, PE_FLAGS_MSG_RECEIVED);

		type = PD_HEADER_TYPE(rx_emsg[port].header);
		cnt = PD_HEADER_CNT(rx_emsg[port].header);
		ext = PD_HEADER_EXT(rx_emsg[port].header);
		sop = PD_HEADER_GET_SOP(rx_emsg[port].header);

		if (ext == 0 && sop == TCPCI_MSG_SOP) {
			if ((cnt > 0) && (type == PD_DATA_SINK_CAP)) {
				uint32_t *payload =
					(uint32_t *)rx_emsg[port].buf;
				uint8_t cap_cnt =
					rx_emsg[port].len / sizeof(uint32_t);

				pe_set_snk_caps(port, cap_cnt, payload);

				dpm_evaluate_sink_fixed_pdo(port, payload[0]);
				pe_set_ready_state(port);
				return;
			} else if (cnt == 0 &&
				   (type == PD_CTRL_REJECT ||
				    type == PD_CTRL_NOT_SUPPORTED)) {
				pe_set_ready_state(port);
				return;
			}
			/* Unexpected messages fall through to soft reset */
		}

		pe_send_soft_reset(port, sop);
		return;
	}

	/*
	 * Transition to PE_[SRC,SNK]_Ready state when:
	 *   1) SenderResponseTimer times out.
	 *   2) Message was discarded.
	 */
	if ((msg_check & PE_MSG_DISCARDED) ||
	    pd_timer_is_expired(port, PE_TIMER_SENDER_RESPONSE))
		pe_set_ready_state(port);
}

static void pe_dr_get_sink_cap_exit(int port)
{
	pe_sender_response_msg_exit(port);
}

/*
 * PE_DR_SNK_Give_Source_Cap
 */
static void pe_dr_snk_give_source_cap_entry(int port)
{
	print_current_state(port);

	/* Send source capabilities. */
	send_source_cap(port);
}

static void pe_dr_snk_give_source_cap_run(int port)
{
	/*
	 * Transition back to PE_SNK_Ready when the Source_Capabilities message
	 * has been successfully sent.
	 *
	 * Get Source Capabilities AMS is uninterruptible, but in case the
	 * partner violates the spec then send a soft reset rather than get
	 * stuck here.
	 */
	if (PE_CHK_FLAG(port, PE_FLAGS_TX_COMPLETE)) {
		PE_CLR_FLAG(port, PE_FLAGS_TX_COMPLETE);
		set_state_pe(port, PE_SNK_READY);
	} else if (PE_CHK_FLAG(port, PE_FLAGS_MSG_DISCARDED)) {
		pe_send_soft_reset(port, TCPCI_MSG_SOP);
	}
}

/*
 * PE_DR_SRC_Get_Source_Cap
 */
static void pe_dr_src_get_source_cap_entry(int port)
{
	print_current_state(port);

	/* Send a Get_Source_Cap Message */
	tx_emsg[port].len = 0;
	send_ctrl_msg(port, TCPCI_MSG_SOP, PD_CTRL_GET_SOURCE_CAP);
	pe_sender_response_msg_entry(port);
}

static void pe_dr_src_get_source_cap_run(int port)
{
	int type;
	int cnt;
	int ext;
	enum pe_msg_check msg_check;

	/*
	 * Check the state of the message sent
	 */
	msg_check = pe_sender_response_msg_run(port);

	/*
	 * Transition to PE_SRC_Ready when:
	 *   1) A Source Capabilities Message is received.
	 *   2) A Reject Message is received.
	 */
	if ((msg_check & PE_MSG_SENT) &&
	    PE_CHK_FLAG(port, PE_FLAGS_MSG_RECEIVED)) {
		PE_CLR_FLAG(port, PE_FLAGS_MSG_RECEIVED);

		type = PD_HEADER_TYPE(rx_emsg[port].header);
		cnt = PD_HEADER_CNT(rx_emsg[port].header);
		ext = PD_HEADER_EXT(rx_emsg[port].header);

		if (ext == 0) {
			if ((cnt > 0) && (type == PD_DATA_SOURCE_CAP)) {
				uint32_t *payload =
					(uint32_t *)rx_emsg[port].buf;

				pd_set_src_caps(port, cnt, payload);

				/*
				 * If we'd prefer to charge from this partner,
				 * then propose a PR swap.
				 */
				if (pd_can_charge_from_device(port, cnt,
							      payload))
					pd_request_power_swap(port);

				/*
				 * Report dual role power capability to the
				 * charge manager if present
				 */
				if (0/*IS_ENABLED(CONFIG_CHARGE_MANAGER)*/ &&
				    pd_get_partner_dual_role_power(port))
					charge_manager_update_dualrole(
						port, CAP_DUALROLE);

				set_state_pe(port, PE_SRC_READY);
			} else if ((cnt == 0) &&
				   (type == PD_CTRL_REJECT ||
				    type == PD_CTRL_NOT_SUPPORTED)) {
				pd_set_src_caps(port, -1, NULL);
				set_state_pe(port, PE_SRC_READY);
			} else {
				/*
				 * On protocol error, consider source cap
				 * retrieval a failure
				 */
				pd_set_src_caps(port, -1, NULL);
				set_state_pe(port, PE_SEND_SOFT_RESET);
			}
			return;
		} else {
			pd_set_src_caps(port, -1, NULL);
			set_state_pe(port, PE_SEND_SOFT_RESET);
			return;
		}
	}

	/*
	 * Transition to PE_SRC_Ready state when:
	 *   1) the SenderResponseTimer times out.
	 *   2) Message was discarded.
	 */
	if ((msg_check & PE_MSG_DISCARDED) ||
	    pd_timer_is_expired(port, PE_TIMER_SENDER_RESPONSE))
		set_state_pe(port, PE_SRC_READY);
}

static void pe_dr_src_get_source_cap_exit(int port)
{
	pe_sender_response_msg_exit(port);
}

/*
 * PE_Get_Revision
 */
__maybe_unused static void pe_get_revision_entry(int port)
{
	print_current_state(port);

	/* Send a Get_Revision message */
	send_ctrl_msg(port, TCPCI_MSG_SOP, PD_CTRL_GET_REVISION);
	pe_sender_response_msg_entry(port);
}

__maybe_unused static void pe_get_revision_run(int port)
{
	int type;
	int cnt;
	int ext;
	enum pe_msg_check msg_check;

	/* Check the state of the message sent */
	msg_check = pe_sender_response_msg_run(port);

	if ((msg_check & PE_MSG_SENT) &&
	    PE_CHK_FLAG(port, PE_FLAGS_MSG_RECEIVED)) {
		PE_CLR_FLAG(port, PE_FLAGS_MSG_RECEIVED);

		type = PD_HEADER_TYPE(rx_emsg[port].header);
		cnt = PD_HEADER_CNT(rx_emsg[port].header);
		ext = PD_HEADER_EXT(rx_emsg[port].header);

		if (ext == 0 && cnt == 1 && type == PD_DATA_REVISION) {
			/* Revision returned by partner */
			pe[port].partner_rmdo =
				*((struct rmdo *)rx_emsg[port].buf);
		} else if (type != PD_CTRL_NOT_SUPPORTED) {
			/*
			 * If the partner response with a message other than
			 * Revision or Not_Supported, there was an interrupt.
			 * Setting PE_FLAGS_MSG_RECEIVED to handle unexpected
			 * message.
			 */
			PE_SET_FLAG(port, PE_FLAGS_MSG_RECEIVED);
		}

		/*
		 * Get_Revision is an interruptible AMS. Return to ready state
		 * after response whether or not there was a protocol error.
		 */
		pe_set_ready_state(port);
		return;
	}

	/*
	 * Return to ready state if the message was discarded or timer expires
	 */
	if ((msg_check & PE_MSG_DISCARDED) ||
	    pd_timer_is_expired(port, PE_TIMER_SENDER_RESPONSE))
		pe_set_ready_state(port);
}

__maybe_unused static void pe_get_revision_exit(int port)
{
	pe_sender_response_msg_exit(port);
}


static void pe_enter_epr_mode(int port)
{
	PE_CLR_FLAG(port, PE_FLAGS_ENTERING_EPR);
	PE_CLR_FLAG(port, PE_FLAGS_EPR_EXPLICIT_EXIT);
	PE_SET_FLAG(port, PE_FLAGS_IN_EPR);
	CPRINTS("C%d: Entered EPR", port);
}

static void pe_exit_epr_mode(int port)
{
	PE_CLR_FLAG(port, PE_FLAGS_IN_EPR);
	PE_CLR_DPM_REQUEST(port, DPM_REQUEST_EPR_MODE_EXIT);
	CPRINTS("C%d: Exited EPR", port);
}

/*
 * PE_SNK_EPR_KEEP_ALIVE
 */
static void pe_snk_epr_keep_alive_entry(int port)
{
	struct pd_ecdb *ecdb = (void *)tx_emsg[port].buf;

	if (pe_debug_level >= DEBUG_LEVEL_2) {
		print_current_state(port);
	}

	ecdb->type = PD_EXT_CTRL_EPR_KEEPALIVE;
	ecdb->data = 0;
	tx_emsg[port].len = sizeof(*ecdb);

	send_ext_data_msg(port, TCPCI_MSG_SOP, PD_EXT_CONTROL);
	pe_sender_response_msg_entry(port);
}

static void pe_snk_epr_keep_alive_run(int port)
{
	enum pe_msg_check msg_check = pe_sender_response_msg_run(port);

	if (msg_check & PE_MSG_DISCARDED) {
		/*
		 * An EPR_KeepAlive was discarded due to an incoming message
		 * from the source. Both ends know the partnership is alive. We
		 * go back to SNK_Ready and restart the KeepAlive timer.
		 */
		set_state_pe(port, PE_SNK_READY);
		return;
	}

	if (msg_check & PE_MSG_SENT &&
	    PE_CHK_FLAG(port, PE_FLAGS_MSG_RECEIVED)) {
		int type = PD_HEADER_TYPE(rx_emsg[port].header);
		int cnt = PD_HEADER_CNT(rx_emsg[port].header);
		int ext = PD_HEADER_EXT(rx_emsg[port].header);
		struct pd_ecdb *ecdb = (void *)rx_emsg[port].buf;

		PE_CLR_FLAG(port, PE_FLAGS_MSG_RECEIVED);

		if (cnt == 0 || ext == 0 || type != PD_EXT_CONTROL) {
			CPRINTS("C%d: Protocol Error: 0x%04x", port,
				rx_emsg[port].header);
			pe_send_soft_reset(port, TCPCI_MSG_SOP);
		} else if (ecdb->type == PD_EXT_CTRL_EPR_KEEPALIVE_ACK) {
			pe_sender_response_msg_exit(port);
			set_state_pe(port, PE_SNK_READY);
		}

		return;
	}

	if (pd_timer_is_expired(port, PE_TIMER_SENDER_RESPONSE))
		pe_set_hard_reset(port);
}

/*
 * PE_SNK_SEND_EPR_MODE_ENTRY
 */
static void pe_snk_send_epr_mode_entry_entry(int port)
{
	struct eprmdo *eprmdo = (void *)tx_emsg[port].buf;

	print_current_state(port);

	PE_SET_FLAG(port, PE_FLAGS_ENTERING_EPR);

	/* Send EPR mode entry message */
	eprmdo->action = PD_EPRMDO_ACTION_ENTER;
	eprmdo->data = 0; /* EPR Sink Operational PDP */
	eprmdo->reserved = 0;
	tx_emsg[port].len = sizeof(*eprmdo);

	send_data_msg(port, TCPCI_MSG_SOP, PD_DATA_EPR_MODE);
	pe_sender_response_msg_entry(port);

	pd_timer_enable(port, PE_TIMER_SINK_EPR_ENTER, PD_T_ENTER_EPR);
}

static void pe_snk_send_epr_mode_entry_run(int port)
{
	enum pe_msg_check msg_check;

	/* Check the state of the message sent */
	msg_check = pe_sender_response_msg_run(port);

	if (msg_check & PE_MSG_DISCARDED) {
		PE_CLR_FLAG(port, PE_FLAGS_ENTERING_EPR);
		set_state_pe(port, PE_SNK_READY);
		return;
	}

	if ((msg_check & PE_MSG_SENT) &&
	    PE_CHK_FLAG(port, PE_FLAGS_MSG_RECEIVED)) {
		uint8_t type = PD_HEADER_TYPE(rx_emsg[port].header);
		uint8_t cnt = PD_HEADER_CNT(rx_emsg[port].header);
		uint8_t ext = PD_HEADER_EXT(rx_emsg[port].header);

		PE_CLR_FLAG(port, PE_FLAGS_MSG_RECEIVED);
		if ((ext == 0) && (cnt > 0) && (type == PD_DATA_EPR_MODE)) {
			struct eprmdo *eprmdo = (void *)rx_emsg[port].buf;

			if (eprmdo->action == PD_EPRMDO_ACTION_ENTER_ACK) {
				/* EPR Enter Mode Acknowledge received */
				set_state_pe(
					port,
					PE_SNK_EPR_MODE_ENTRY_WAIT_FOR_RESPONSE);
				return;
			}
			/*
			 * Other actions should result in soft reset but not
			 * clear from the spec. So, we just let it time out.
			 */
		}
	}

	/* When the SinkEPREnterTimer times out, send a soft reset. */
	if (pd_timer_is_expired(port, PE_TIMER_SINK_EPR_ENTER)) {
		pe_send_soft_reset(port, TCPCI_MSG_SOP);
	} else if (pd_timer_is_expired(port, PE_TIMER_SENDER_RESPONSE)) {
		pe_send_soft_reset(port, TCPCI_MSG_SOP);
	}
}

static void pe_snk_send_epr_mode_entry_exit(int port)
{
	pe_sender_response_msg_exit(port);
}

/*
 * PE_SNK_EPR_MODE_ENTRY_WAIT_FOR_RESPONSE
 */
static void pe_snk_epr_mode_entry_wait_for_response_entry(int port)
{
	print_current_state(port);
	/* Wait for EPR Enter Mode response */
}

static void pe_snk_epr_mode_entry_wait_for_response_run(int port)
{
	if (PE_CHK_FLAG(port, PE_FLAGS_MSG_RECEIVED)) {
		uint8_t type = PD_HEADER_TYPE(rx_emsg[port].header);
		uint8_t cnt = PD_HEADER_CNT(rx_emsg[port].header);
		uint8_t ext = PD_HEADER_EXT(rx_emsg[port].header);

		PE_CLR_FLAG(port, PE_FLAGS_MSG_RECEIVED);
		if ((ext == 0) && (cnt > 0) && (type == PD_DATA_EPR_MODE)) {
			struct eprmdo *eprmdo = (void *)rx_emsg[port].buf;

			if (eprmdo->action == PD_EPRMDO_ACTION_ENTER_SUCCESS) {
				pe_enter_epr_mode(port);
				set_state_pe(port,
					     PE_SNK_WAIT_FOR_CAPABILITIES);
				return;
			} else if (eprmdo->action ==
				   PD_EPRMDO_ACTION_ENTER_FAILED) {
				PE_CLR_FLAG(port, PE_FLAGS_ENTERING_EPR);
				/* Table 6-50 EPR Mode Data Object */
				CPRINTS("C%d: Failed to enter EPR for 0x%x",
					port, eprmdo->data);
			}
			/* Fall through to soft reset. */
		} else if ((ext == 0) && (cnt == 0) &&
			   (type == PD_CTRL_VCONN_SWAP)) {
			set_state_pe(port, PE_VCS_EVALUATE_SWAP);
			return;
		}
		/*
		 * 6.4.10.1 Process to enter EPR Mode
		 * "3. If the Sink receives any Message, other than an
		 * EPR_ModeMessage with ENTER_SUCCESS, the Sink Shall initiate a
		 * Soft Reset."
		 */
		pe_send_soft_reset(port, TCPCI_MSG_SOP);
		return;
	}

	/* When the SinkEPREnterTimer times out, send a soft reset. */
	if (pd_timer_is_expired(port, PE_TIMER_SINK_EPR_ENTER)) {
		PE_SET_FLAG(port, PE_FLAGS_SNK_WAIT_CAP_TIMEOUT);
		pe_send_soft_reset(port, TCPCI_MSG_SOP);
	}
}

static void pe_snk_epr_mode_entry_wait_for_response_exit(int port)
{
	pd_timer_disable(port, PE_TIMER_SINK_EPR_ENTER);
	/*
	 * Figure 8-215 indicates a sink shall enter EPR Mode on exit but Figure
	 * 6-34 indicates we enter EPR mode only on success (and soft reset
	 * otherwise). Since the later makes sense, we don't enter EPR here.
	 */
}

/*
 * PE_SNK_SEND_EPR_MODE_EXIT
 */
static void pe_snk_send_epr_mode_exit_entry(int port)
{
	struct eprmdo *eprmdo = (void *)tx_emsg[port].buf;

	print_current_state(port);

	/* Send EPR mode entry message */
	eprmdo->action = PD_EPRMDO_ACTION_EXIT;
	eprmdo->data = 0;
	eprmdo->reserved = 0;
	tx_emsg[port].len = sizeof(*eprmdo);

	send_data_msg(port, TCPCI_MSG_SOP, PD_DATA_EPR_MODE);
	pe_sender_response_msg_entry(port);
}

static void pe_snk_send_epr_mode_exit_run(int port)
{
	enum pe_msg_check msg_check = pe_sender_response_msg_run(port);

	if (msg_check & PE_MSG_DISCARDED) {
		set_state_pe(port, PE_SNK_READY);
		return;
	}

	if (msg_check & PE_MSG_SENT) {
		pe_sender_response_msg_exit(port);
		pe_exit_epr_mode(port);
		set_state_pe(port, PE_SNK_WAIT_FOR_CAPABILITIES);
	}
}

/*
 * PE_SNK_EPR_MODE_EXIT_RECEIVED
 */
static void pe_snk_epr_mode_exit_received_entry(int port)
{
	print_current_state(port);

	/*
	 * Table 8-22 Steps for Exiting EPR Mode (Source Initiated) states 'The
	 * Port Partners are in an Explicit Contract using an SPR PDO.' Thus,
	 * it's expected Source already has sent new SPR PDOs (and we switched
	 * to a SPR contract) before it sent EPR mode exit. Violation of this
	 * results in a hard reset (6.4.10.3.3 Exits due to errors).
	 */
	if (!pe_in_spr_contract(port)) {
		CPRINTS("C%d: Received EPR exit while in EPR contract", port);
		pe_set_hard_reset(port);
		return;
	}

	pe_exit_epr_mode(port);
	set_state_pe(port, PE_SNK_WAIT_FOR_CAPABILITIES);
}

const uint32_t *const pd_get_src_caps(int port)
{
	return pe[port].src_caps;
}

void pd_set_src_caps(int port, int cnt, uint32_t *src_caps)
{
	const int limit = ARRAY_SIZE(pe[port].src_caps);
	int i;

	if (cnt > limit) {
		CPRINTS("C%d: Trim PDOs (%d) exceeding limit (%d)", port, cnt,
			limit);
		cnt = limit;
	}

	pe[port].src_cap_cnt = cnt;

	for (i = 0; i < cnt; i++)
		pe[port].src_caps[i] = *src_caps++;
}

uint8_t pd_get_src_cap_cnt(int port)
{
	if (pe[port].src_cap_cnt > 0)
		return pe[port].src_cap_cnt;

	return 0;
}

/* Track access to the PD discovery structures during HC execution */
atomic_t task_access[CONFIG_USB_PD_PORT_MAX_COUNT][DISCOVERY_TYPE_COUNT];

void pd_dfp_discovery_init(int port)
{
	atomic_or(&task_access[port][TCPCI_MSG_SOP], BIT(task_get_current()));
	atomic_or(&task_access[port][TCPCI_MSG_SOP_PRIME],
		  BIT(task_get_current()));

	memset(pe[port].discovery, 0, sizeof(pe[port].discovery));
}

void pd_dfp_mode_init(int port)
{
	PE_CLR_FLAG(port, PE_FLAGS_MODAL_OPERATION);

	/* Reset the DPM and DP modules to enable alternate mode entry. */
	dpm_mode_exit_complete(port);
	dp_init(port);

	if (0/*IS_ENABLED(CONFIG_USB_PD_TBT_COMPAT_MODE)*/)
		tbt_init(port);

	if (0/*IS_ENABLED(CONFIG_USB_PD_USB4)*/)
		enter_usb_init(port);

	if (0/*IS_ENABLED(CONFIG_USB_PD_ALT_MODE_UFP_DP)*/)
		pd_ufp_set_dp_opos(port, 0);
}

__maybe_unused void pd_discovery_access_clear(int port,
					      enum tcpci_msg_type type)
{
	if (!0/*IS_ENABLED(CONFIG_USB_PD_ALT_MODE_DFP)*/)
		assert(0);

	atomic_clear_bits(&task_access[port][type], 0xFFFFFFFF);
}

__maybe_unused bool pd_discovery_access_validate(int port,
						 enum tcpci_msg_type type)
{
	if (!0/*IS_ENABLED(CONFIG_USB_PD_ALT_MODE_DFP)*/)
		assert(0);

	return !(task_access[port][type] & ~BIT(task_get_current()));
}

__maybe_unused struct pd_discovery *
pd_get_am_discovery_and_notify_access(int port, enum tcpci_msg_type type)
{
	atomic_or(&task_access[port][type], BIT(task_get_current()));
	return (struct pd_discovery *)pd_get_am_discovery(port, type);
}

__maybe_unused const struct pd_discovery *
pd_get_am_discovery(int port, enum tcpci_msg_type type)
{
	if (!0/*IS_ENABLED(CONFIG_USB_PD_ALT_MODE_DFP)*/)
		assert(0);
	ASSERT(type < DISCOVERY_TYPE_COUNT);

	return &pe[port].discovery[type];
}

__maybe_unused void pd_set_dfp_enter_mode_flag(int port, bool set)
{
	if (!0/*IS_ENABLED(CONFIG_USB_PD_ALT_MODE_DFP)*/)
		assert(0);

	if (set)
		PE_SET_FLAG(port, PE_FLAGS_MODAL_OPERATION);
	else
		PE_CLR_FLAG(port, PE_FLAGS_MODAL_OPERATION);
}

const char *pe_get_current_state(int port)
{
	if (pe_is_running(port) && IS_ENABLED(USB_PD_DEBUG_LABELS))
		return pe_state_names[get_state_pe(port)];
	else
		return "";
}

uint32_t pe_get_flags(int port)
{
	/*
	 * TODO(b/229655319): support more than 32 bits
	 */
	return pe[port].flags_a[0];
}

static __const_data const struct usb_state pe_states[] = {
/* Super States */
	[PE_PRS_FRS_SHARED] = {
		.entry = pe_prs_frs_shared_entry,
		.exit  = pe_prs_frs_shared_exit,
	},
	[PE_SNK_STARTUP] = {
		.entry = pe_snk_startup_entry,
		.run = pe_snk_startup_run,
	},
	[PE_SNK_DISCOVERY] = {
		.entry = pe_snk_discovery_entry,
		.run = pe_snk_discovery_run,
	},
	[PE_SNK_WAIT_FOR_CAPABILITIES] = {
		.entry = pe_snk_wait_for_capabilities_entry,
		.run = pe_snk_wait_for_capabilities_run,
		.exit = pe_snk_wait_for_capabilities_exit,
	},
	[PE_SNK_EVALUATE_CAPABILITY] = {
		.entry = pe_snk_evaluate_capability_entry,
	},
	[PE_SNK_SELECT_CAPABILITY] = {
		.entry = pe_snk_select_capability_entry,
		.run = pe_snk_select_capability_run,
		.exit = pe_snk_select_capability_exit,
	},
	[PE_SNK_READY] = {
		.entry = pe_snk_ready_entry,
		.run   = pe_snk_ready_run,
		.exit   = pe_snk_ready_exit,
	},
	[PE_SNK_HARD_RESET] = {
		.entry = pe_snk_hard_reset_entry,
		.run   = pe_snk_hard_reset_run,
	},
	[PE_SNK_TRANSITION_TO_DEFAULT] = {
		.entry = pe_snk_transition_to_default_entry,
		.run   = pe_snk_transition_to_default_run,
	},
	[PE_SNK_GIVE_SINK_CAP] = {
		.entry = pe_snk_give_sink_cap_entry,
		.run = pe_snk_give_sink_cap_run,
	},
	[PE_SNK_GET_SOURCE_CAP] = {
		.entry = pe_snk_get_source_cap_entry,
		.run   = pe_snk_get_source_cap_run,
	},
	[PE_SNK_TRANSITION_SINK] = {
		.entry = pe_snk_transition_sink_entry,
		.run   = pe_snk_transition_sink_run,
		.exit   = pe_snk_transition_sink_exit,
	},
	[PE_SEND_SOFT_RESET] = {
		.entry = pe_send_soft_reset_entry,
		.run = pe_send_soft_reset_run,
		.exit = pe_send_soft_reset_exit,
	},
	[PE_SOFT_RESET] = {
		.entry = pe_soft_reset_entry,
		.run = pe_soft_reset_run,
	},
	[PE_SEND_NOT_SUPPORTED] = {
		.entry = pe_send_not_supported_entry,
		.run = pe_send_not_supported_run,
	},
	[PE_SRC_PING] = {
		.entry = pe_src_ping_entry,
		.run   = pe_src_ping_run,
	},
	[PE_DRS_EVALUATE_SWAP] = {
		.entry = pe_drs_evaluate_swap_entry,
		.run   = pe_drs_evaluate_swap_run,
	},
	[PE_DRS_CHANGE] = {
		.entry = pe_drs_change_entry,
		.run   = pe_drs_change_run,
	},
	[PE_DRS_SEND_SWAP] = {
		.entry = pe_drs_send_swap_entry,
		.run   = pe_drs_send_swap_run,
		.exit  = pe_drs_send_swap_exit,
	},
	[PE_PRS_SRC_SNK_EVALUATE_SWAP] = {
		.entry = pe_prs_src_snk_evaluate_swap_entry,
		.run   = pe_prs_src_snk_evaluate_swap_run,
	},
	[PE_PRS_SRC_SNK_TRANSITION_TO_OFF] = {
		.entry = pe_prs_src_snk_transition_to_off_entry,
		.run   = pe_prs_src_snk_transition_to_off_run,
		.exit  = pe_prs_src_snk_transition_to_off_exit,
	},
	[PE_PRS_SRC_SNK_ASSERT_RD] = {
		.entry = pe_prs_src_snk_assert_rd_entry,
		.run   = pe_prs_src_snk_assert_rd_run,
	},
	[PE_PRS_SRC_SNK_WAIT_SOURCE_ON] = {
		.entry = pe_prs_src_snk_wait_source_on_entry,
		.run   = pe_prs_src_snk_wait_source_on_run,
		.exit  = pe_prs_src_snk_wait_source_on_exit,
	},
	[PE_PRS_SRC_SNK_SEND_SWAP] = {
		.entry = pe_prs_src_snk_send_swap_entry,
		.run   = pe_prs_src_snk_send_swap_run,
		.exit  = pe_prs_src_snk_send_swap_exit,
	},
	[PE_PRS_SNK_SRC_EVALUATE_SWAP] = {
		.entry = pe_prs_snk_src_evaluate_swap_entry,
		.run   = pe_prs_snk_src_evaluate_swap_run,
	},
	/*
	 * Some of the Power Role Swap actions are shared with the very
	 * similar actions of Fast Role Swap.
	 */
	/* State actions are shared with PE_FRS_SNK_SRC_TRANSITION_TO_OFF */
	[PE_PRS_SNK_SRC_TRANSITION_TO_OFF] = {
		.entry = pe_prs_snk_src_transition_to_off_entry,
		.run   = pe_prs_snk_src_transition_to_off_run,
		.exit  = pe_prs_snk_src_transition_to_off_exit,
		.parent = &pe_states[PE_PRS_FRS_SHARED],
	},
	/* State actions are shared with PE_FRS_SNK_SRC_ASSERT_RP */
	[PE_PRS_SNK_SRC_ASSERT_RP] = {
		.entry = pe_prs_snk_src_assert_rp_entry,
		.run   = pe_prs_snk_src_assert_rp_run,
		.parent = &pe_states[PE_PRS_FRS_SHARED],
	},
	/* State actions are shared with PE_FRS_SNK_SRC_SOURCE_ON */
	[PE_PRS_SNK_SRC_SOURCE_ON] = {
		.entry = pe_prs_snk_src_source_on_entry,
		.run   = pe_prs_snk_src_source_on_run,
		.exit  = pe_prs_snk_src_source_on_exit,
		.parent = &pe_states[PE_PRS_FRS_SHARED],
	},
	/* State actions are shared with PE_FRS_SNK_SRC_SEND_SWAP */
	[PE_PRS_SNK_SRC_SEND_SWAP] = {
		.entry = pe_prs_snk_src_send_swap_entry,
		.run   = pe_prs_snk_src_send_swap_run,
		.exit  = pe_prs_snk_src_send_swap_exit,
		.parent = &pe_states[PE_PRS_FRS_SHARED],
	},
	[PE_DEU_SEND_ENTER_USB] = {
		.entry = pe_enter_usb_entry,
		.run = pe_enter_usb_run,
		.exit = pe_enter_usb_exit,
	},
	[PE_WAIT_FOR_ERROR_RECOVERY] = {
		.entry = pe_wait_for_error_recovery_entry,
		.run   = pe_wait_for_error_recovery_run,
	},
	[PE_BIST_TX] = {
		.entry = pe_bist_tx_entry,
		.run   = pe_bist_tx_run,
		.exit  = pe_bist_tx_exit,
	},
	[PE_DR_GET_SINK_CAP] = {
		.entry = pe_dr_get_sink_cap_entry,
		.run   = pe_dr_get_sink_cap_run,
		.exit  = pe_dr_get_sink_cap_exit,
	},
	[PE_DR_SNK_GIVE_SOURCE_CAP] = {
		.entry = pe_dr_snk_give_source_cap_entry,
		.run = pe_dr_snk_give_source_cap_run,
	},
	[PE_DR_SRC_GET_SOURCE_CAP] = {
		.entry = pe_dr_src_get_source_cap_entry,
		.run   = pe_dr_src_get_source_cap_run,
		.exit  = pe_dr_src_get_source_cap_exit,
	},
	[PE_FRS_SNK_SRC_START_AMS] = {
		.entry = pe_frs_snk_src_start_ams_entry,
		.parent = &pe_states[PE_PRS_FRS_SHARED],
	},
	[PE_GET_REVISION] = {
		.entry = pe_get_revision_entry,
		.run   = pe_get_revision_run,
		.exit  = pe_get_revision_exit,
	},
	[PE_SRC_CHUNK_RECEIVED] = {
		.entry = pe_chunk_received_entry,
		.run   = pe_chunk_received_run,
		.exit  = pe_chunk_received_exit,
	},
	[PE_SNK_CHUNK_RECEIVED] = {
		.entry = pe_chunk_received_entry,
		.run   = pe_chunk_received_run,
		.exit  = pe_chunk_received_exit,
	},
	[PE_SNK_EPR_KEEP_ALIVE] = {
		.entry = pe_snk_epr_keep_alive_entry,
		.run = pe_snk_epr_keep_alive_run,
	},
	[PE_SNK_SEND_EPR_MODE_ENTRY] = {
		.entry = pe_snk_send_epr_mode_entry_entry,
		.run = pe_snk_send_epr_mode_entry_run,
		.exit = pe_snk_send_epr_mode_entry_exit,
	},
	[PE_SNK_EPR_MODE_ENTRY_WAIT_FOR_RESPONSE] = {
		.entry = pe_snk_epr_mode_entry_wait_for_response_entry,
		.run = pe_snk_epr_mode_entry_wait_for_response_run,
		.exit = pe_snk_epr_mode_entry_wait_for_response_exit,
	},
	[PE_SNK_SEND_EPR_MODE_EXIT] = {
		.entry = pe_snk_send_epr_mode_exit_entry,
		.run = pe_snk_send_epr_mode_exit_run,
	},
	[PE_SNK_EPR_MODE_EXIT_RECEIVED] = {
		.entry = pe_snk_epr_mode_exit_received_entry,
	},
};

#ifdef TEST_BUILD
/* TODO(b/173791979): Unit tests shouldn't need to access internal states */
const struct test_sm_data test_pe_sm_data[] = {
	{
		.base = pe_states,
		.size = ARRAY_SIZE(pe_states),
		.names = pe_state_names,
		.names_size = ARRAY_SIZE(pe_state_names),
	},
};
BUILD_ASSERT(ARRAY_SIZE(pe_states) == ARRAY_SIZE(pe_state_names));
const int test_pe_sm_data_size = ARRAY_SIZE(test_pe_sm_data);

void pe_set_fn(int port, int fn)
{
	PE_SET_FN(port, fn);
}
void pe_clr_fn(int port, int fn)
{
	PE_CLR_FN(port, fn);
}
int pe_chk_fn(int port, int fn)
{
	return PE_CHK_FN(port, fn);
}
void pe_clr_dpm_requests(int port)
{
	pe[port].dpm_request = 0;
}
#endif
