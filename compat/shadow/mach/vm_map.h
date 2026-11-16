/* Non-Apple hosts: the slice of Darwin's vm_map.h that
   devices/winvirt.c's hv_alloc_no_footprint() uses.
 *
 * On iOS that function takes memory out of the app's Jetsam footprint by
 * owning the memory entry itself. There is no Jetsam here, so a plain
 * anonymous mapping is exactly right and the ownership call is a no-op. */
#ifndef HENGE_COMPAT_MACH_VM_MAP_H
#define HENGE_COMPAT_MACH_VM_MAP_H

#include <sys/mman.h>
#include <stdint.h>

#include "mach/mach.h"

#ifndef KERN_SUCCESS
#define KERN_SUCCESS 0
#endif
#ifndef KERN_FAILURE
#define KERN_FAILURE 5
#endif
#ifndef VM_FLAGS_ANYWHERE
#define VM_FLAGS_ANYWHERE 0x0001
#endif
#ifndef VM_FLAGS_OVERWRITE
#define VM_FLAGS_OVERWRITE 0x4000
#endif
#ifndef VM_PROT_READ
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#endif

typedef uint64_t mach_vm_address_t;
typedef uint64_t mach_vm_size_t;

static inline int vm_allocate(mach_port_t task, void **addr, size_t size, int flags) {
    (void)task, (void)flags;
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return KERN_FAILURE;
    *addr = p;
    return KERN_SUCCESS;
}

static inline int vm_deallocate(mach_port_t task, void *addr, size_t size) {
    (void)task;
    return munmap(addr, size) == 0 ? KERN_SUCCESS : KERN_FAILURE;
}

#endif
