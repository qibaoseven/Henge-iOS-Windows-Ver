/* Non-Apple hosts (Linux / iSH / Alpine): the handful of Darwin services that
 * host/winvm.c reaches for, for when the machine is a headless runner rather
 * than an iOS app.
 *
 * There is no CoreFoundation, no os/lock.h and no QoS here, so:
 *
 *   CFTypeRef / CFRetain / CFRelease   become an untyped pointer and no-ops.
 *                                      A display layer handed in on iOS is a
 *                                      CAMetalLayer; here there is none, and
 *                                      hv_winvm_set_display_layer(NULL) is the
 *                                      only call the runner makes.
 *   os_unfair_lock                     becomes a pthread mutex (they are both
 *                                      non-recursive and not async-signal-safe,
 *                                      and struct layout is private either way).
 *   pthread_set_qos_class_self_np      does nothing: Linux scheduling is the
 *                                      caller's business.
 *   <sys/ucontext.h>                   musl has the same ucontext_t for the
 *                                      signal-handler stub winvm.c installs.
 *
 * Included with -include from compat/build-and-test.sh, before the sources.
 */
#ifndef HENGE_PORT_DARWIN_SHIM_H
#define HENGE_PORT_DARWIN_SHIM_H

#ifndef __APPLE__

#include <pthread.h>
#include <stddef.h>

typedef const void *CFTypeRef;
#define CFRetain(x) ((CFTypeRef)(x))
#define CFRelease(x) ((void)(x))

typedef struct {
    pthread_mutex_t m;
} os_unfair_lock, *os_unfair_lock_t;

#define OS_UNFAIR_LOCK_INIT {PTHREAD_MUTEX_INITIALIZER}

static inline void os_unfair_lock_lock(os_unfair_lock *l) { pthread_mutex_lock(&l->m); }
static inline void os_unfair_lock_unlock(os_unfair_lock *l) { pthread_mutex_unlock(&l->m); }

typedef int qos_class_t;
#define QOS_CLASS_USER_INITIATED 1
#define QOS_CLASS_USER_INTERACTIVE 2
#define QOS_CLASS_DEFAULT 0

static inline int pthread_set_qos_class_self_np(qos_class_t c, int prio) {
    (void)c, (void)prio;
    return 0;
}

/* copyfile()/removefile() live in libSystem on Darwin. share.c uses them to
   move a file in or out of the shared folder; this host has no shared folder
   (there is no UI to browse it), so the shim is only here to keep the file
   compiling if it is ever built. */
typedef struct _copyfile_state *copyfile_state_t;
#define COPYFILE_DATA 0x0008
#define COPYFILE_ALL 0x07B8
#define COPYFILE_ACL 0x0001
#define COPYFILE_STAT 0x0002
#define COPYFILE_XATTR 0x0004
#define COPYFILE_EXCL 0x0001

#endif /* !__APPLE__ */

#endif /* HENGE_PORT_DARWIN_SHIM_H */
