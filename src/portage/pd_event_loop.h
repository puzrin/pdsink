#ifndef PD_EVENT_LOOP_H
#define PD_EVENT_LOOP_H

/* Timer expired. */
#define TASK_EVENT_TIMER (1U << 31)
/* Maximum time for task_wait_event() */
#define TASK_MAX_WAIT_US 0x7fffffff

void pd_event_loop(int port);

#endif // PD_EVENT_LOOP_H