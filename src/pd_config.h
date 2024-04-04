#define CONFIG_USB_PD_EPR
#define CONFIG_USB_PD_REV30

// This should be still ok for ERP, because required messages
// are transferred in single chunk. But should be checked for sure.
#undef CONFIG_USB_PD_EXTENDED_MESSAGES

#undef CONFIG_ZEPHYR
#undef CONFIG_USBC_VCONN
#undef CONFIG_USB_PD_EXTENDED_MESSAGES
#undef CONFIG_USB_PD_DATA_RESET_MSG
