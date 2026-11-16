/* Non-Apple hosts: Darwin's pthread/qos.h. Linux scheduling classes are the
   caller's business, so the one call the engine makes does nothing. */
#ifndef HENGE_COMPAT_PTHREAD_QOS_H
#define HENGE_COMPAT_PTHREAD_QOS_H
typedef int qos_class_t;
#define QOS_CLASS_DEFAULT 0
#define QOS_CLASS_USER_INITIATED 1
#define QOS_CLASS_USER_INTERACTIVE 2
#define QOS_CLASS_UTILITY 3
#define QOS_CLASS_BACKGROUND 4
static inline int pthread_set_qos_class_self_np(qos_class_t c, int prio) {
    (void)c, (void)prio;
    return 0;
}
#endif
