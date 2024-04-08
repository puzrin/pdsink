Common

- DRP
- USB
- Try to remove VBUS checks.


Scheduler:

- Port param (instead of task ID)
- Re-enterance barrier
- Periodic 5ms timer to poll DPM <=> PE communications
  - Should kick all scheduler instances.

Timer:

- Use 1ms interrupots, instead of individual intervals calc.
- Still may need 300uS pauses if VBUS voltage measure used in FUSB302.

Driver's protothreads:

- Prevent conflicts with outer app. Hide under namespace or prefixes.
- Cleanup: use only `goto LABEL` approach for context switch.
- If timeout needed - move local static var into ptt context.
- Scheduler re-enterance barrier for sure.


