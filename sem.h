#ifndef SEM_H
#define SEM_H

#include <stdint.h>

/* A small set of global counting semaphores identified by a fixed id, shared by
 * all processes (System V style). Blocking waits use the task scheduler's
 * block/wake mechanism, so a process waiting on an empty semaphore yields the
 * CPU until another process posts it. */
#define MAX_SEMAPHORES 8

/* Initialise semaphore `id` with the given count. Returns 0, or -1 on bad id. */
int sem_init(int id, int value);
/* P: block until the count is positive, then decrement. Returns 0, or -1. */
int sem_wait(int id);
/* V: increment the count and wake one waiter. Returns 0, or -1 on error
 * (including a count already at INT32_MAX). */
int sem_post(int id);

#endif
