#include "fw_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* All three registers are big endian in QEMU (DEVICE_BIG_ENDIAN) and so here:
   the guest writes the selector as a 16-bit big-endian store and reads bytes.
   An access the real device would refuse (a selector that is not 2 bytes, data
   that is not at +0x00) is ignored rather than faulted: this is a device the
   firmware finds by looking, and answering nonsense to a probe would panic it
   in a way that says nothing. */

static void put16be(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static void put32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static const HvFwCfgFile *find(HvFwCfg *c, uint16_t sel) {
    for (int i = 0; i < c->nfiles; i++)
        if (c->file[i].select == sel) return &c->file[i];
    return NULL;
}

/* The bytes the selected entry answers from, and how long it is. */
static const uint8_t *entry_bytes(HvFwCfg *c, uint16_t sel, uint64_t *len) {
    static uint8_t sig[4] = {'Q', 'E', 'M', 'U'};
    static uint8_t id[4] = {0x01, 0x00, 0x00, 0x00}; /* VERSION: revision 1, no features */
    *len = 0;
    switch (sel) {
    case HV_FWCFG_SIGNATURE: *len = 4; return sig;
    case HV_FWCFG_ID: *len = 4; return id;
    case HV_FWCFG_FILE_DIR: {
        if (!c->dir) hv_fw_cfg_finalize(c);
        *len = c->dir_len;
        return c->dir;
    }
    default: break;
    }
    const HvFwCfgFile *f = find(c, sel);
    if (!f) return NULL;
    *len = f->len;
    return f->data;
}

static uint64_t fw_read(void *opaque, uint64_t off, unsigned size) {
    HvFwCfg *c = opaque;
    if (off == 0x08) return 0; /* the selector reads back 0 in QEMU */
    if (off == 0x10) return c->dma;
    if (off != 0 || size != 1) return 0; /* data: one byte at a time */
    uint64_t len = 0;
    const uint8_t *b = entry_bytes(c, c->sel, &len);
    uint8_t v = 0;
    if (b && c->pos < len) v = b[c->pos];
    c->pos++;
    return v;
}

static void fw_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvFwCfg *c = opaque;
    if (off == 0x08) {
        if (size != 2) return; /* QEMU accepts nothing else */
        /* Big endian on the wire. Selecting an entry restarts it even when it is
           the one already selected: QEMU's fw_cfg_select() always rewinds. */
        uint16_t sel = (uint16_t)(((v & 0xff) << 8) | ((v >> 8) & 0xff));
        c->sel = sel;
        c->pos = 0;
        return;
    }
    if (off == 0x10) {
        c->dma = v;
        if (c->m->log) fprintf(stderr, "hengevm: fw_cfg: a DMA write was ignored (no DMA; see fw_cfg.h)\n");
        return;
    }
    /* data register: writes were removed in QEMU 2.4 and this understands none */
}

HvFwCfg *hv_fw_cfg_create(HvMachine *m, uint64_t base) {
    HvFwCfg *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->m = m;
    c->base = base;
    c->size = 0x18; /* data 0x00, selector 0x08, DMA 0x10 */
    if (hv_machine_add_mmio(m, base, c->size, c, fw_read, fw_write, "fw_cfg") != 0) {
        free(c);
        return NULL;
    }
    return c;
}

void hv_fw_cfg_destroy(HvFwCfg *c) {
    if (!c) return;
    for (int i = 0; i < c->nfiles; i++) free(c->file[i].data);
    free(c->dir);
    free(c);
}

int hv_fw_cfg_add_file(HvFwCfg *c, const char *name, const void *data, uint64_t len) {
    if (c->nfiles == HV_FWCFG_MAX_FILES) return -1;
    if (strlen(name) >= HV_FWCFG_NAME) return -1;
    HvFwCfgFile *f = &c->file[c->nfiles];
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", name);
    f->len = len;
    f->data = malloc(len ? (size_t)len : 1);
    if (!f->data) return -1;
    if (len) memcpy(f->data, data, (size_t)len);
    f->select = (uint16_t)(HV_FWCFG_FIRST_FILE + c->nfiles * 2); /* QEMU: two selectors per file, the second unused */
    c->nfiles++;
    c->dir_len = 4 + (uint64_t)c->nfiles * 64;
    free(c->dir);
    c->dir = NULL;
    return 0;
}

void hv_fw_cfg_finalize(HvFwCfg *c) {
    free(c->dir);
    c->dir_len = 4 + (uint64_t)c->nfiles * 64;
    c->dir = calloc(1, (size_t)c->dir_len);
    if (!c->dir) {
        c->dir_len = 0;
        return;
    }
    put32be(c->dir, (uint32_t)c->nfiles);
    for (int i = 0; i < c->nfiles; i++) {
        uint8_t *e = c->dir + 4 + (uint64_t)i * 64;
        put32be(e, (uint32_t)c->file[i].len);      /* size */
        put16be(e + 4, c->file[i].select);          /* select */
        e[6] = 0, e[7] = 0;                         /* reserved */
        memcpy(e + 8, c->file[i].name, HV_FWCFG_NAME);
    }
}
