/* Non-Apple hosts (Linux/Android): what core/ takes from <mach/mach_time.h>.
 *
 * The guest's counter is mach_absolute_time's, and on Apple silicon that ticks
 * at 24 MHz. Everywhere else a monotonic clock scaled to 24 MHz gives the same
 * number the translated MRS reads, so code that assumes 24 MHz (sysreg.c,
 * dispatch.c's 50 us/1 ms windows) keeps its meaning.
 *
 * Only for building the engine off Apple platforms; the Apple build uses the
 * real header. */
#ifndef HENGE_PORT_MACH_TIME_H
#define HENGE_PORT_MACH_TIME_H

#define HENGE_COUNTER_HZ 24000000ull

#include <stdint.h>
#include <time.h>
#include <sys/time.h>

/* C11 + musl: clock_gettime only appears with a POSIX feature macro, and the
   engine's own headers are included without one. */
static inline uint64_t henge_host_ticks(void) {
    struct timeval tv;
    gettimeofday(&tv, 0); /* CLOCK_MONOTONIC keeps running across settimeofday; the
                             difference only shifts time by the host's NTP step, which
                             a guest sees as its clock stepping anyway */
    return (uint64_t)tv.tv_sec * HENGE_COUNTER_HZ + (uint64_t)tv.tv_usec * (HENGE_COUNTER_HZ / 1000000ull);
}

static inline uint64_t mach_absolute_time(void) { return henge_host_ticks(); }

#endif
