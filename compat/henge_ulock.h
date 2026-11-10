/* Non-Apple hosts (Linux/Android): Darwin's __ulock_wait/__ulock_wake, on top
 * of Linux's futex.
 *
 * core/machine.c keeps its cross-CPU acknowledgement word (HvPerCpu.ack_seq)
 * with these: a CPU waiting for another's TLBI acknowledgement sleeps on the
 * word's low 32 bits and is woken when it moves, which is compare-and-wait on
 * a futex word exactly. The operations core/ uses:
 *
 *   __ulock_wait(UL_COMPARE_AND_WAIT, addr, expected, timeout_us)
 *       sleeps while *(uint32_t *)addr == expected, at most timeout_us
 *   __ulock_wake(UL_COMPARE_AND_WAIT | ULF_WAKE_ALL, addr, 0)
 *       wakes everyone sleeping on addr
 *
 * Only for building the engine off Apple platforms. */
#ifndef HENGE_PORT_ULOCK_H
#define HENGE_PORT_ULOCK_H

#include <errno.h>
#include <limits.h>
/* musl has no linux/futex.h installed; the two operations core/ needs. */
#ifndef FUTEX_WAIT_PRIVATE
#define FUTEX_WAIT_PRIVATE 128
#define FUTEX_WAKE_PRIVATE 129
#endif
#include <stdint.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define HV_UL_COMPARE_AND_WAIT 1u
#define HV_ULF_WAKE_ALL 0x100u
#define HV_ULF_NO_ERRNO 0x1000000u

static inline int henge_futex(uint32_t *addr, int op, uint32_t val, const struct timespec *to) {
    return (int)syscall(SYS_futex, addr, op, val, to, NULL, 0);
}

/* Compare against the low word, the way Darwin's does: the high word (the
   sequence number's upper half) is free to move on its own. */
static inline int __ulock_wait(uint32_t operation, void *addr, uint64_t value, uint32_t timeout_us) {
    (void)operation;
    uint32_t *w = (uint32_t *)addr;
    if (__atomic_load_n(w, __ATOMIC_SEQ_CST) != (uint32_t)value) return -1; /* EALREADY: it moved already */
    struct timespec ts;
    struct timespec *tp = NULL;
    if (timeout_us != 0) {
        ts.tv_sec = (time_t)(timeout_us / 1000000u);
        ts.tv_nsec = (long)(timeout_us % 1000000u) * 1000l;
        tp = &ts;
    }
    /* FUTEX_WAIT can report a spurious wakeup; the caller checks its own
       condition, so any return is fine. */
    henge_futex(w, FUTEX_WAIT_PRIVATE, (uint32_t)value, tp);
    return 0;
}

static inline int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value) {
    (void)wake_value;
    uint32_t *w = (uint32_t *)addr;
    int all = (operation & HV_ULF_WAKE_ALL) != 0;
    return henge_futex(w, all ? FUTEX_WAKE_PRIVATE : FUTEX_WAKE_PRIVATE, all ? INT_MAX : 1, NULL);
}

#endif
