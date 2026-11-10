/* Non-Apple hosts (Linux/Android): the Apple platform selects host/jit_region.c
 * and the device sources test with.
 *
 * Everything is 0: not macOS, not iOS, not a simulator. Code that picks an
 * Apple-only path by these then takes the generic one. */
#ifndef HENGE_PORT_TARGETCONDITIONALS_H
#define HENGE_PORT_TARGETCONDITIONALS_H

#define TARGET_OS_MAC 0
#define TARGET_OS_OSX 0
#define TARGET_OS_IPHONE 0
#define TARGET_OS_IOS 0
#define TARGET_OS_SIMULATOR 0
#define TARGET_OS_EMBEDDED 0
#define TARGET_OS_TV 0
#define TARGET_OS_WATCH 0
#define TARGET_OS_VISION 0
#define TARGET_OS_DRIVERKIT 0
#define TARGET_OS_MACCATALYST 0

#endif
