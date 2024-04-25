@@ @@
- #include "assert.h"

@@ @@
- #include "atomic.h"

@@ @@
- #include "atomic_bit.h"

@@ @@
- #include "common.h"

@@ @@
- #include "limits.h"

@@ @@
- #include "math_util.h"

@@ @@
- #include "usb_tc_sm.h"

@@ @@
#include "usb_pd_timer.h"

+ #include "pd_config.h"
+ #include <stdatomic.h>
+
+ /* Workaround, since coccinelle can't patch ATOMIC_DEFINE lines directly */
+ #define ATOMIC_DEFINE(VAR, SIZE) ATOMIC_BOOLS_DEFINE(VAR, SIZE)


@@ expression E; @@
- #define PD_SET_ACTIVE(p, bit) E
+ #define PD_SET_ACTIVE(p, bit) atomic_store(&timer_active[(p) * PD_TIMER_COUNT + (bit)], true)

@@ expression E; @@
- #define PD_CLR_ACTIVE(p, bit) E
+ #define PD_CLR_ACTIVE(p, bit) atomic_store(&timer_active[(p) * PD_TIMER_COUNT + (bit)], false)

@@ expression E; @@
- #define PD_CHK_ACTIVE(p, bit) E
+ #define PD_CHK_ACTIVE(p, bit) atomic_load(&timer_active[(p) * PD_TIMER_COUNT + (bit)])

@@ expression E; @@
- #define PD_SET_DISABLED(p, bit) E
+ #define PD_SET_DISABLED(p, bit) atomic_store(&timer_disabled[(p) * PD_TIMER_COUNT + (bit)], true)

@@ expression E; @@
- #define PD_CLR_DISABLED(p, bit) E
+ #define PD_CLR_DISABLED(p, bit) atomic_store(&timer_disabled[(p) * PD_TIMER_COUNT + (bit)], false)

@@ expression E; @@
- #define PD_CHK_DISABLED(p, bit) E
+ #define PD_CHK_DISABLED(p, bit) atomic_load(&timer_disabled[(p) * PD_TIMER_COUNT + (bit)])


@@ @@
- int pd_timer_next_expiration(int port) { ... }