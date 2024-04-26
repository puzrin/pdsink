#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "src/pd_config.h"
#include "src/portage/pd_loop.h"

#define MAX_PD_PORTS CONFIG_USB_PD_PORT_MAX_COUNT

// Declare arrays of atomic flags for each port
static atomic_flag is_running[MAX_PD_PORTS] = {[0 ... MAX_PD_PORTS - 1] = ATOMIC_FLAG_INIT};
static atomic_flag deferred_call[MAX_PD_PORTS] = {[0 ... MAX_PD_PORTS - 1] = ATOMIC_FLAG_INIT};

// Events storage
static atomic_uint_fast32_t events[MAX_PD_PORTS] = {[0 ... MAX_PD_PORTS - 1] = 0};

static void loop(int port)
{
	/* pick available events */
	const uint32_t evt = atomic_exchange(&events[port], 0);

	if (evt & TASK_EVENT_TIMER) pd_timer_manage_expired(port);

	dpm_run(port, evt, tc_get_pd_enabled(port));
	pe_run(port, evt, tc_get_pd_enabled(port));
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
static void pd_loop(int port) {
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

/*
 * Send event to event loop handler.
 */
void pd_loop_set_event(int port, uint32_t event) {
	atomic_fetch_or(&events[port], event);
	pd_loop(port);
}

/*
 * Timer interrupt handler. Propagate timer event to all ports.
 */
void pd_loop_handle_timer_interrupt() {
	for (int port = 0; port < MAX_PD_PORTS; port++) {
		pd_loop_set_event(port, TASK_EVENT_TIMER);
	}
}