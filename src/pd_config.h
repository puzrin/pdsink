/**
 * Global defines for Google's EC USB Power Delivery stack
 * Those are used for 2 goals
 *
 * 1. Strip unuded stuff by souces fetcher (see `/support` dir), to shrink
 *    files and simplify porting.
 * 2. Code build.
*/


#define CONFIG_USB_PD_EPR
#define CONFIG_USB_PD_REV30

// This should be still ok for ERP, because required messages
// are transferred in single chunk. But should be checked for sure.
#undef CONFIG_USB_PD_EXTENDED_MESSAGES
// Not needed for Sink mode
#undef CONFIG_USB_PD_FRS
#undef CONFIG_USBC_VCONN
#undef CONFIG_USB_PD_DATA_RESET_MSG
#undef CONFIG_AP_POWER_CONTROL
#undef CONFIG_TEMP_SENSOR
#undef CONFIG_USB_PD_USB4
#undef CONFIG_USB_PD_TBT_COMPAT_MODE
#undef CONFIG_USB_PD_ALT_MODE_UFP_DP
#undef CONFIG_USB_PD_ALT_MODE_DFP
#undef CONFIG_CHARGE_MANAGER
#undef CONFIG_USB_PD_RESET_MIN_BATT_SOC
// Seems we can't use this stuff, it's for console only.
#undef CONFIG_USB_PD_PRL_EVENT_LOG

// Drivers
#undef CONFIG_USB_PD_VBUS_DETECT_TCPC
#undef CONFIG_USB_PD_TCPC_LOW_POWER
#undef CONFIG_USB_PD_DECODE_SOP
// tcpci
#undef CONFIG_USB_PD_DUAL_ROLE_AUTO_TOGGLE
#undef CONFIG_USB_PD_TCPM_MUX
#undef CONFIG_CMD_TCPC_DUMP
#undef CONFIG_USB_PD_DISCHARGE_TCPC
#undef CONFIG_USB_PD_FRS_TCPC

// Other
#undef CONFIG_ZEPHYR
