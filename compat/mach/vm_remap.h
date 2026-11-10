/* Non-Apple hosts (Linux/Android): Darwin's vm_remap, as host/jit_region.c
 * uses it, on top of what Linux has.
 *
 * vm_remap makes a second mapping of the *same physical pages* at another
 * address, so a write through one is a write through the other. Linux's
 * equivalent is one shared file description mapped twice -- the file is an
 * unlinked temporary, so nothing survives the process. That is exactly the
 * shape the split-W^X code buffer needs: an RW alias to emit through and an
 * RX alias to run through.
 *
 * open_split maps the region read-write first, then calls this with that
 * address as `source`; the original mapping is replaced in place by the
 * read-execute half of the same file, so the caller's `rx` pointer stays valid.
 *
 * Only for building the engine off Apple platforms. */
#ifndef HENGE_PORT_VM_REMAP_H
#define HENGE_PORT_VM_REMAP_H

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef unsigned int mach_port_t;
typedef uintptr_t vm_address_t;
typedef int vm_prot_t;
typedef int vm_inherit_t;
typedef uint32_t vm_flags_t;

#ifndef KERN_SUCCESS
#define KERN_SUCCESS 0
#endif
#ifndef KERN_FAILURE
#define KERN_FAILURE 1
#endif
#define KERN_INVALID_ARGUMENT 4

#define VM_PROT_NONE 0x0
#define VM_PROT_READ 0x1
#define VM_PROT_WRITE 0x2
#define VM_PROT_EXECUTE 0x4

#define VM_FLAGS_ANYWHERE 0x0001
#define VM_FLAGS_OVERWRITE 0x4000
#define VM_INHERIT_NONE 0

#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif

/* vm_remap(target_task, &target_address, size, mask, flags, src_task,
            source_address, copy, &cur_prot, &max_prot, inherit)
   `source_address` is an existing read-write mapping of `size` bytes. */
static inline int vm_remap(mach_port_t target_task, vm_address_t *target_address, size_t size, size_t mask,
                           vm_flags_t flags, mach_port_t src_task, vm_address_t source_address, int copy,
                           vm_prot_t *cur_protection, vm_prot_t *max_protection, vm_inherit_t inheritance) {
    (void)target_task, (void)mask, (void)flags, (void)src_task, (void)copy, (void)inheritance;
    int prot = (cur_protection && *cur_protection) ? *cur_protection : (VM_PROT_READ | VM_PROT_WRITE);
    void *from = (void *)source_address;

    /* An unlinked temp: both mappings keep the file alive, and closing every
       descriptor still leaves the pages. */
    char path[] = "/tmp/henge-jit-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return KERN_FAILURE;
    unlink(path);
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return KERN_FAILURE;
    }

    /* The writable alias, wherever the kernel puts it. */
    void *to = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (to == MAP_FAILED) {
        close(fd);
        return KERN_FAILURE;
    }
    /* The caller's original mapping becomes the executable half, in place. */
    munmap(from, size);
    if (mmap(from, size, prot | PROT_EXEC, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
        munmap(to, size);
        close(fd);
        return KERN_FAILURE;
    }
    close(fd); /* the mappings keep the file */
    *target_address = (vm_address_t)to;
    if (cur_protection) *cur_protection = prot;
    if (max_protection) *max_protection = prot;
    return KERN_SUCCESS;
}

#endif
