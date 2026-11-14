/* Non-Apple hosts (Linux/Android): Darwin's <libkern/OSCacheControl.h>, which
 * keeps the instruction cache in step with code the translator wrote.
 *
 * aarch64 in userspace cannot run "ic ivau" (EL1 or above), so the flush goes
 * through the kernel: __clear_cache is GCC/Clang's builtin for exactly this,
 * and on Linux it issues the maintenance the CPU needs.
 *
 * Only for building the engine off Apple platforms; the Apple build uses the
 * real header. */
#ifndef HENGE_PORT_OSCACHECONTROL_H
#define HENGE_PORT_OSCACHECONTROL_H

#include <stddef.h>

static inline void sys_icache_invalidate(void *start, size_t len) {
    extern void __clear_cache(void *begin, void *end);
    __clear_cache(start, (char *)start + len);
}

static inline void sys_dcache_flush(void *start, size_t len) {
    (void)start;
    (void)len;
}

#endif
