#ifndef PD_LOOP_H
#define PD_LOOP_H

/* pd_loop_wake() called */
#define TASK_EVENT_WAKE BIT(29)
/* Timer expired. */
#define TASK_EVENT_TIMER (1U << 31)
/* Maximum time for task_wait_event() */
#define TASK_MAX_WAIT_US 0x7fffffff

/*
 * Send event to event loop handler.
 */
void pd_loop_set_event(int port,  uint32_t event);

/*
 * Wake up the event loop.
 */
static inline void pd_loop_wake(int port)
{
    pd_loop_set_event(port, TASK_EVENT_WAKE);
}

/*
 * Timer interrupt handler. Propagate timer event to all ports.
 */
void pd_loop_handle_timer_interrupt();

#endif // PD_LOOP_H