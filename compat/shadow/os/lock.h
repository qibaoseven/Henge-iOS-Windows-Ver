/* Non-Apple hosts: Darwin's os/lock.h, as the engine uses it (a non-recursive
   mutex with a static initialiser). */
#ifndef HENGE_COMPAT_OS_LOCK_H
#define HENGE_COMPAT_OS_LOCK_H
#include <pthread.h>
typedef struct { pthread_mutex_t m; } os_unfair_lock, *os_unfair_lock_t;
#define OS_UNFAIR_LOCK_INIT {PTHREAD_MUTEX_INITIALIZER}
static inline void os_unfair_lock_lock(os_unfair_lock *l) { pthread_mutex_lock(&l->m); }
static inline void os_unfair_lock_unlock(os_unfair_lock *l) { pthread_mutex_unlock(&l->m); }
#endif
