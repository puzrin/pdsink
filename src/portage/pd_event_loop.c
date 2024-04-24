#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "src/pd_config.h"
#include "src/portage/pd_event_loop.h"

#define MAX_PD_PORTS CONFIG_USB_PD_PORT_MAX_COUNT

// Declare arrays of atomic flags for each port
static atomic_flag is_running[MAX_PD_PORTS] = {[0 ... MAX_PD_PORTS - 1] = ATOMIC_FLAG_INIT};
static atomic_flag deferred_call[MAX_PD_PORTS] = {[0 ... MAX_PD_PORTS - 1] = ATOMIC_FLAG_INIT};


static void loop(int port)
{
	/* wait for next event/packet or timeout expiration */
	const uint32_t evt = task_wait_event(pd_task_timeout(port));

	/* Manage expired PD Timers on timeouts */
	if (evt & TASK_EVENT_TIMER)
		pd_timer_manage_expired(port);

	/*
	 * run port controller task to check CC and/or read incoming
	 * messages
	 */
	tcpc_run(port, evt);

	/* Run Device Policy Manager */
	dpm_run(port, evt, tc_get_pd_enabled(port));

	/* Run policy engine state machine */
	pe_run(port, evt, tc_get_pd_enabled(port));

	/* Run protocol state machine */
	prl_run(port, evt, tc_get_pd_enabled(port));

	return true;
}

/*
 * Since we don't use RTOS, we need our own event loop.
 *
 * This function should be called in 2 cases:
 *
 * 1. Every 1-5 ms, when underlying abstractions use deferred signaling
 * 2. On every immediate signaling.
 *
 * NOTE: there is chance to call this every 0.1ms, to support good timeouts
 * resolution.
 */
void pd_event_loop(int port) {
	/* Wrapper to flatten nested invocations. Real logic is in `loop()` */
    bool should_run = false;

    do {
        if (atomic_flag_test_and_set(&is_running[port])) {
            atomic_flag_test_and_set(&deferred_call[port]);
            return;
        }

        loop(port);

        atomic_flag_clear(&is_running[port]);

        if (atomic_flag_test_and_set(&deferred_call[port])) {
            atomic_flag_clear(&deferred_call[port]);
            should_run = true;
        } else {
            should_run = false;
        }

    } while (should_run);
}
