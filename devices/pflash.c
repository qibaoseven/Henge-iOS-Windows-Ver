#include "pflash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* nor flash semantics at the window level: the code bank ignores writes, and
   bytes past what a file filled read as erased. Henge does not model the command
   set (CFI): UEFI reaches the variables through the second bank directly, and
   the code bank is never erased or programmed at run time. */

static uint64_t bank_read(void *opaque, uint64_t off, unsigned size) {
    HvPflash *p = opaque;
    /* The window is [0, size0 + size1): everything below size0 is the code bank,
       whatever the images' own lengths are. */
    int bank = off >= p->size[0];
    uint64_t boff = off - (bank ? p->size[0] : 0), size_of = p->size[bank];
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        uint8_t b = 0xff; /* erased */
        if (boff + i < size_of) b = p->mem[bank][boff + i];
        v |= (uint64_t)b << (8 * i);
    }
    return v;
}

static void bank_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvPflash *p = opaque;
    if (off < p->size[0]) return; /* the code bank: read-only */
    uint64_t boff = off - p->size[0], size_of = p->size[1];
    for (unsigned i = 0; i < size; i++) {
        if (boff + i >= size_of) break;
        uint8_t b = (uint8_t)(v >> (8 * i));
        /* NOR programming can only clear bits; erasing is the guest's job and
           UEFI's variable driver does it through this same window. Modelling it
           as a plain store is what its flash driver expects of a device it can
           write at all. */
        p->mem[1][boff + i] = b;
    }
    hv_dma_written(p->m, p->base[1] + boff, size);
}

static int load(const char *path, uint8_t *to, uint64_t cap, uint64_t *got) {
    *got = 0;
    if (!path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "hengevm: cannot open flash image %s\n", path);
        return -1;
    }
    /* read(2), not fread: a short fread() zero-fills the rest of the buffer it
       was given (musl does; it is within what the standard permits), which would
       turn the erased 0xff tail into zeros. The tail must stay erased. */
    int fd = fileno(f);
    ssize_t n = 0, r = 0;
    while ((uint64_t)n < cap && (r = read(fd, to + n, (size_t)(cap - (uint64_t)n))) > 0) n += r;
    if (r < 0) {
        fprintf(stderr, "hengevm: reading flash image %s: error\n", path);
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
        /* Erased: every byte 0xff. A file fills from the start of its bank. */
        p->mem[b] = malloc((size_t)p->size[b]);
        if (!p->mem[b]) goto fail;
        memset(p->mem[b], 0xff, (size_t)p->size[b]);
    }
    uint64_t got = 0;
    if (load(code_path, p->mem[0], p->size[0], &got) != 0) goto fail;
    if (got) fprintf(stderr, "hengevm: code flash %s, %llu bytes at %llx\n", code_path,
                     (unsigned long long)got, (unsigned long long)pa);
    if (load(vars_path, p->mem[1], p->size[1], &got) != 0) goto fail;
    if (got) fprintf(stderr, "hengevm: vars flash %s, %llu bytes at %llx\n", vars_path,
                     (unsigned long long)got, (unsigned long long)p->base[1]);
    if (hv_machine_add_mmio(m, pa, code_size + vars_size, p, bank_read, bank_write, "pflash") != 0) goto fail;
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
