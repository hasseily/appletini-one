#ifndef ONEWWW_TASK_H
#define ONEWWW_TASK_H

/* One cooperative task with its own stack (CPU0, ARM only).
 *
 * The page layout is deeply recursive and can need more than a second.
 * It runs as a task: when its time slice ends it calls
 * onewww_task_yield(), and the main loop continues with its other
 * services. The next onewww_task_resume() continues the layout at the
 * same point. The task never runs at the same time as the main loop, so
 * the two need no locks. */
typedef void (*onewww_task_fn)(void);

/* Prepares the task. It starts at the first resume. */
void onewww_task_start(onewww_task_fn function);
/* Runs the task until it yields or ends. Returns 1 while it is not ended. */
int onewww_task_resume(void);
/* Called in the task: goes back to the caller of onewww_task_resume(). */
void onewww_task_yield(void);
int onewww_task_active(void);

#endif
