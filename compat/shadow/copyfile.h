/* Non-Apple hosts: Darwin's copyfile(3) and removefile(3).
 *
 * host/share.c moves files in and out of the guest's WebDAV folder with them.
 * On this host there is no shared folder to browse (the iOS UI is what offers
 * it), so the two entry points exist only to keep the file compiling; both
 * report failure, which share.c already treats as "the copy did not happen". */
#ifndef HENGE_COMPAT_COPYFILE_H
#define HENGE_COMPAT_COPYFILE_H

#include <errno.h>

typedef struct _copyfile_state *copyfile_state_t;

#define COPYFILE_ACL 0x0001
#define COPYFILE_STAT 0x0002
#define COPYFILE_XATTR 0x0004
#define COPYFILE_DATA 0x0008
#define COPYFILE_EXCL 0x0001
#define COPYFILE_RECURSIVE 0x8000
#define COPYFILE_ALL (COPYFILE_ACL | COPYFILE_STAT | COPYFILE_XATTR | COPYFILE_DATA)

static inline int copyfile(const char *from, const char *to, copyfile_state_t s, int flags) {
    (void)from, (void)to, (void)s, (void)flags;
    errno = ENOSYS;
    return -1;
}

#define REMOVEFILE_RECURSIVE 0x0001
static inline int removefile(const char *path, copyfile_state_t s, int flags) {
    (void)path, (void)s, (void)flags;
    errno = ENOSYS;
    return -1;
}

#endif
