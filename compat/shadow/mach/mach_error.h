/* Non-Apple hosts: Darwin's mach_error.h, which only names error strings.
   Nothing on the Windows path decodes a mach error code; the runner prints
   errno instead, so a declaration is all the caller needs. */
#ifndef HENGE_COMPAT_MACH_ERROR_H
#define HENGE_COMPAT_MACH_ERROR_H

typedef int mach_error_t;

static inline char *mach_error_string(mach_error_t e) {
    (void)e;
    return (char *)"mach error";
}

#endif
