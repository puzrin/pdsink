@@ expression port; @@
- task_wake(PD_PORT_TO_TASK_ID(port))
+ pd_loop_wake(port)

@@ expression port, evt; @@
- task_set_event(PD_PORT_TO_TASK_ID(port), evt)
+ pd_loop_set_event(port, evt)
