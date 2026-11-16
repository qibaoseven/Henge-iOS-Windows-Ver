/* Non-Apple hosts: Darwin's removefile(3) lives here on macOS; the two entry
 * points are declared in compat/shadow/copyfile.h, which this includes. */
#ifndef HENGE_COMPAT_REMOVEFILE_H
#define HENGE_COMPAT_REMOVEFILE_H
#include <copyfile.h>
#endif
