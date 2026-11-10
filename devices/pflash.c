#include "pflash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* RAM, not a device (see pflash.h). Nor-flash semantics are kept only where
   they matter to the firmware: the code bank ignores writes, and bytes past what
   a file filled read as erased 0xff. The command set (CFI) is not modelled --
   UEFI reaches its variables through the writable bank as ordinary memory, and
   the code bank is never erased or programmed at run time. */

static int load(const char *path, uint8_t *to, uint64_t cap, uint64_t *got) {
    *got = 0;
    if (!path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "hengevm: cannot open flash image %s\n", path);
        return -1;
    }
    /* read(2), not fread: a short fread() zero-fills the rest of the buffer it
       was given (musl does, which the standard permits), and the erased tail
       must stay 0xff. */
    int fd = fileno(f);
    ssize_t n = 0, r = 0;
    while ((uint64_t)n < cap && (r = read(fd, to + n, (size_t)(cap - (uint64_t)n))) > 0) n += r;
    if (r < 0) {
        fprintf(stderr, "hengevm: reading flash image %s failed\n", path);
        fclose(f);
        return -1;
    }
    int more = 0;
    if ((uint64_t)n == cap) {
        uint8_t extra;
        more = read(fd, &extra, 1) > 0;
    }
    fclose(f);
    if (more) {
        fprintf(stderr, "hengevm: flash image %s is larger than its bank (%llu bytes)\n", path,
                (unsigned long long)cap);
        return -1;
    }
    *got = (uint64_t)n;
    return 0;
}

HvPflash *hv_pflash_create(HvMachine *m, uint64_t pa, uint64_t code_size, uint64_t vars_size,
                           const char *code_path, const char *vars_path) {
    HvPflash *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->m = m;
    p->base[0] = pa, p->size[0] = code_size;
    p->base[1] = pa + code_size, p->size[1] = vars_size;
    for (int b = 0; b < 2; b++) {
        p->mem[b] = malloc((size_t)p->size[b]);
        if (!p->mem[b]) goto fail;
        memset(p->mem[b], 0xff, (size_t)p->size[b]); /* erased */
    }
    uint64_t got = 0;
    if (load(code_path, p->mem[0], p->size[0], &got) != 0) goto fail;
    if (got) fprintf(stderr, "hengevm: code flash %s, %llu bytes at %llx\n", code_path,
                     (unsigned long long)got, (unsigned long long)pa);
    if (load(vars_path, p->mem[1], p->size[1], &got) != 0) goto fail;
    if (got) fprintf(stderr, "hengevm: vars flash %s, %llu bytes at %llx\n", vars_path,
                     (unsigned long long)got, (unsigned long long)p->base[1]);
    /* Code bank read-only, vars bank writable: the engine keeps writes out of a
       read-only region, so no device callback is needed. */
    if (hv_machine_add_ram(m, p->base[0], p->size[0], p->mem[0], 1) != 0) goto fail;
    if (hv_machine_add_ram(m, p->base[1], p->size[1], p->mem[1], 0) != 0) goto fail;
    return p;
fail:
    hv_pflash_destroy(p);
    return NULL;
}

void hv_pflash_destroy(HvPflash *p) {
    if (!p) return;
    free(p->mem[0]);
    free(p->mem[1]);
    free(p);
}

int hv_pflash_save_vars(HvPflash *p, const char *path) {
    if (!p || !path) return -1;
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "hengevm: cannot write %s\n", path);
        return -1;
    }
    size_t n = fwrite(p->mem[1], 1, (size_t)p->size[1], f);
    int bad = n != p->size[1];
    if (fclose(f) != 0) bad = 1;
    return bad ? -1 : 0;
}
