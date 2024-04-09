@@
// Clear struct fields from non-sink stuff.
// Unused functions will be removed on next passes.

identifier N = { release, get_vbus_voltage, select_rp_value, set_vconn,
    get_rp_value, set_roles, set_vconn_cb, set_vconn_discharge_cb,
    vconn_discharge, dump_std_reg, set_bist_test_mode, sop_prime_enable,
    check_vbus_level, get_chip_info, reset_bist_type_2, set_bist_test_mode };
type T;
identifier I;
expresstion E;
@@
T I = {
  ...,
- .N = (...),
+ .N = NULL,
  ...
};


@@
// Force remove some public functions, not used in Sink.

identifier N = { fusb302_get_vbus_voltage, tcpm_set_bist_test_data };
type T;
@@
- T N(...) { ... }


@@
// Hide SRC-only branch
@@
-	detect_cc_pin_source_manual(port, cc1, cc2);
+	assert(0); /* [hide to reduce code size] detect_cc_pin_source_manual(port, cc1, cc2); */
