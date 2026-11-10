/* Non-Apple hosts (Linux/Android): the pthread extensions core/ uses that
 * glibc and musl do not have.
 *
 * pthread_cond_timedwait_relative_np is the Darwin one: an absolute deadline
 * against the monotonic clock, relative to now. Linux spells the same wait
 * with pthread_cond_timedwait plus CLOCK_MONOTONIC timestamps, which is what
 * this does. The engine includes this after <pthread.h>.
 *
 * Only for building the engine off Apple platforms. */
#ifndef HENGE_PORT_PTHREAD_H
#define HENGE_PORT_PTHREAD_H

#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <time.h>

/* musl/glibc hide clock_gettime and CLOCK_MONOTONIC behind _POSIX_C_SOURCE,
   and this header lands before the engine's sources pick one. Take the
   monotonic clock the way every libc still exports it. */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
extern int clock_gettime(int clock_id, struct timespec *tp);

static inline int pthread_cond_timedwait_relative_np(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *rel) {
    struct timespec abs;
    clock_gettime(CLOCK_MONOTONIC, &abs);
    abs.tv_sec += rel->tv_sec;
    abs.tv_nsec += rel->tv_nsec;
    if (abs.tv_nsec >= 1000000000l) {
        abs.tv_sec += 1;
        abs.tv_nsec -= 1000000000l;
    }
    return pthread_cond_timedwait(cond, mutex, &abs);
}

#endif
