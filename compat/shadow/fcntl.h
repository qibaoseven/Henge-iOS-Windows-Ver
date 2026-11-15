/* No Darwin: F_NOCACHE and F_BARRIERFSYNC do not exist. host/disk.c guards the
   calls that use them, and this gives the guard something to compile against. */
#ifndef HENGE_COMPAT_FCNTL_H
#define HENGE_COMPAT_FCNTL_H
#include_next <fcntl.h>
#ifndef F_NOCACHE
#define F_NOCACHE 48
#endif
#ifndef F_BARRIERFSYNC
#define F_BARRIERFSYNC 85
#endif
#endif
