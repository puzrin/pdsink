/* Copyright 2020 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* TCPM for STM32Gx UCPD module */

#include "chip/stm32/ucpd-stm32gx.h"
#include "common.h"
#include "config.h"
#include "console.h"
#include "hooks.h"
#include "registers.h"
#include "stm32gx.h"
#include "system.h"
#include "task.h"
#include "tcpm/tcpci.h"
#include "timer.h"
#include "usb_pd.h"
#include "usb_pd_tcpm.h"
#include "util.h"

/*
 * STM32G4 UCPD peripheral does not have the ability to detect VBUS, but
 * CONFIG_USB_PD_VBUS_DETECT_TCPC maybe still be defined for another port on the
 * same board which uses a TCPC that does have this feature. Therefore, this
 * config option is not considered an error.
 */

#define CPRINTF(format, args...) cprintf(CC_USBPD, format, ##args)
#define CPRINTS(format, args...) cprints(CC_USBPD, format, ##args)

/* Wait time for vconn power switch to turn off. */
#ifndef PD_STM32GX_VCONN_TURN_OFF_DELAY_US
#define PD_STM32GX_VCONN_TURN_OFF_DELAY_US 500
#endif

static int cached_rp[CONFIG_USB_PD_PORT_MAX_COUNT];

static int stm32gx_tcpm_get_message_raw(int port, uint32_t *buf, int *head)
{
	return stm32gx_ucpd_get_message_raw(port, buf, head);
}

static int stm32gx_tcpm_init(int port)
{
	return stm32gx_ucpd_init(port);
}

static int stm32gx_tcpm_get_cc(int port, enum tcpc_cc_voltage_status *cc1,
			       enum tcpc_cc_voltage_status *cc2)
{
	/* Get cc_state value for each CC line */
	stm32gx_ucpd_get_cc(port, cc1, cc2);

	return EC_SUCCESS;
}

static int stm32gx_tcpm_set_cc(int port, int pull)
{
	return stm32gx_ucpd_set_cc(port, pull, cached_rp[port]);
}

static int stm32gx_tcpm_set_polarity(int port, enum tcpc_cc_polarity polarity)
{
	return stm32gx_ucpd_set_polarity(port, polarity);
}

static int stm32gx_tcpm_set_msg_header(int port, int power_role, int data_role)
{
	return stm32gx_ucpd_set_msg_header(port, power_role, data_role);
}

static int stm32gx_tcpm_set_rx_enable(int port, int enable)
{
	return stm32gx_ucpd_set_rx_enable(port, enable);
}

static int stm32gx_tcpm_transmit(int port, enum tcpci_msg_type type,
				 uint16_t header, const uint32_t *data)
{
	return stm32gx_ucpd_transmit(port, type, header, data);
}

static void stm32gx_tcpm_sw_reset(void)
{
	/*
	 * TODO(b/167601672): Not sure if this hook is required for UCPD as
	 * opposed to TCPCI compliant TCPC. Leaving this a placeholder so I
	 * don't forget to pull this back in, if required.
	 */
}
DECLARE_HOOK(HOOK_USB_PD_DISCONNECT, stm32gx_tcpm_sw_reset, HOOK_PRIO_DEFAULT);

enum ec_error_list stm32gx_tcpm_set_bist_test_mode(const int port,
						   const bool enable)
{
	return stm32gx_ucpd_set_bist_test_mode(port, enable);
}

bool stm32gx_tcpm_check_vbus_level(int port, enum vbus_level level)
{
	/*
	 * UCPD peripheral can't detect VBUS, so always return 0. Any port which
	 * uses the stm32g4 UCPD peripheral for its TCPC would also have a PPC
	 * that will handle VBUS detection. However, there may be products which
	 * don't have a PPC on some ports that will rely on a TCPC to do VBUS
	 * detection.
	 */
	return 0;
}

const struct tcpm_drv stm32gx_tcpm_drv = {
	.init = &stm32gx_tcpm_init,
	.release = NULL,
	.get_cc = &stm32gx_tcpm_get_cc,
	.check_vbus_level = NULL,
	.select_rp_value = NULL,
	.set_cc = &stm32gx_tcpm_set_cc,
	.set_polarity = &stm32gx_tcpm_set_polarity,
	.set_vconn = NULL,
	.set_msg_header = &stm32gx_tcpm_set_msg_header,
	.set_rx_enable = &stm32gx_tcpm_set_rx_enable,
	.get_message_raw = &stm32gx_tcpm_get_message_raw,
	.transmit = &stm32gx_tcpm_transmit,
	.get_chip_info = NULL,
	.reset_bist_type_2 = NULL,
	.set_bist_test_mode = NULL,
};
