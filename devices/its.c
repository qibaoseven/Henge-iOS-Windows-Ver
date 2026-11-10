#include "its.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The ITS, as the architecture leaves it to the implementation.

   Registers (4 KB apart, ignoring the per-collection ones this does not use):
     +0x0000 GITS_CTLR      [0] Enable
     +0x0008 GITS_TYPER     read-only
     +0x0040 GITS_TRANSLATER a device's MSI writes here
     +0x0080 GITS_CBASER    [0] valid, [11:0] size-1 in 4 KB pages, [63:12] base
     +0x0090 GITS_CREADR    maintained here (where the next command is)
     +0x0098 GITS_CWRITER   the guest writes where it stopped writing commands

   GITS_TYPER says: 8-byte translation table entries, 16-bit EventIDs,
   24-bit DeviceIDs, 256 physical LPIs from INTID 8192, no virtual LPIs. */

#define R_CTLR 0x0000
#define R_TYPER 0x0008
#define R_TRANSLATER 0x0040
#define R_CBASER 0x0080
#define R_CREADR 0x0090
#define R_CWRITER 0x0098

/* Commands (the 8-bit opcode is bits [31:24] of the first word). */
#define OP_MOVI 0x01
#define OP_SYNC 0x04
#define OP_MAPD 0x08
#define OP_MAPC 0x09
#define OP_MAPI 0x0a
#define OP_MAPTI 0x0b
#define OP_DISCARD 0x0f

static uint64_t rd64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static struct HvItsDev *find_dev(HvIts *its, uint32_t device, int make) {
    for (int i = 0; i < its->ndev; i++)
        if (its->dev[i].valid && its->dev[i].device == device) return &its->dev[i];
    if (!make) return NULL;
    if (its->ndev == its->capdev) {
        int cap = its->capdev ? its->capdev * 2 : 8;
        void *p = realloc(its->dev, (size_t)cap * sizeof *its->dev);
        if (!p) return NULL;
        its->dev = p;
        its->capdev = cap;
    }
    struct HvItsDev *d = &its->dev[its->ndev++];
    memset(d, 0, sizeof *d);
    d->device = device;
    d->valid = 1;
    d->collection = -1;
    return d;
}

static void map_event(HvIts *its, uint32_t device, uint32_t event, uint32_t lpi) {
    if (lpi < HV_ITS_LPI_BASE || lpi >= HV_ITS_LPI_BASE + HV_ITS_LPI_COUNT) return; /* not ours */
    struct HvItsDev *d = find_dev(its, device, 1);
    if (!d) return;
    for (int i = 0; i < d->nev; i++)
        if (d->ev[i].event == event) {
            d->ev[i].lpi = (int)(lpi - HV_ITS_LPI_BASE);
            return;
        }
    if (d->nev < (int)(sizeof d->ev / sizeof d->ev[0])) {
        d->ev[d->nev].event = event;
        d->ev[d->nev].lpi = (int)(lpi - HV_ITS_LPI_BASE);
        d->nev++;
    }
}

/* The CPU a collection's LPIs land on: set by MAPC, and by the redistributor
   address it names (the machine lays them out one after another, so the index
   of the one whose range holds that address is the CPU). */
static int coll_cpu(HvIts *its, int coll) {
    if (coll < 0 || coll >= HV_MAX_CPUS) return -1;
    return its->coll[coll];
}

/* An LPI goes to the CPU its collection names. */
static void pend_lpi(HvIts *its, int lpi, int cpu) {
    if (lpi < 0 || lpi >= (int)HV_ITS_LPI_COUNT) return;
    if (cpu < 0 || cpu >= HV_MAX_CPUS) return;
    hv_gicv3_pend_cpu(its->gic, (int)hv_its_intid(lpi), cpu);
}

static void do_cmd(HvIts *its, uint64_t w0, uint64_t w1, uint64_t w2) {
    uint32_t op = (uint32_t)(w0 >> 24) & 0xff;
    switch (op) {
    case OP_MAPD: { /* [63:32] DeviceID, [10:8] CollectionID */
        struct HvItsDev *d = find_dev(its, (uint32_t)(w0 >> 32), 1);
        if (d) d->collection = (int)((w0 >> 8) & 0xff);
        break;
    }
    case OP_MAPC: { /* [63:32] RDbase, [15:0] CollectionID */
        uint32_t rd = (uint32_t)(w0 >> 32);
        uint32_t coll = (uint32_t)(w0 & 0xffff);
        if (coll < HV_MAX_CPUS) {
            its->coll[coll] = its->rd_cpu ? its->rd_cpu(rd) : 0;
            if ((int)coll + 1 > its->ncoll) its->ncoll = (int)coll + 1;
        }
        break;
    }
    case OP_MAPTI: /* [63:32] DeviceID, [31:16] EventID, [15:0] physical LPI */
    case OP_MAPI:
        map_event(its, (uint32_t)(w0 >> 32), (uint32_t)((w0 >> 16) & 0xff), (uint32_t)(w0 & 0xffff));
        break;
    case OP_MOVI: { /* [63:32] DeviceID, [15:0] EventID: pend it on its collection */
        struct HvItsDev *d = find_dev(its, (uint32_t)(w0 >> 32), 0);
        uint32_t event = (uint32_t)(w0 & 0xffff);
        if (!d) break;
        int cpu = coll_cpu(its, d->collection);
        if (cpu < 0) break;
        for (int i = 0; i < d->nev; i++)
            if (d->ev[i].event == event) {
                pend_lpi(its, d->ev[i].lpi, cpu);
                break;
            }
        break;
    }
    case OP_DISCARD: {
        struct HvItsDev *d = find_dev(its, (uint32_t)(w0 >> 32), 0);
        if (d) d->nev = 0;
        break;
    }
    default: break; /* SYNC, MOVALL, INV, INVALL: nothing is cached here to invalidate */
    }
}

static void run_commands(HvIts *its, uint32_t write) {
    /* GITS_CBASER: [7:0] size - 1 in 4 KB pages, [51:12] base, [63] valid.
       (The architecture puts valid at bit 63, not bit 0: a queue written the
       other way reads as one page at an address of zero.) */
    if (!(its->ctlr & 1) || !(its->cbaser >> 63)) return;
    uint64_t bytes = (uint64_t)((its->cbaser & 0xff) + 1) << 12;
    if (!bytes) return;
    uint8_t *q = hv_phys_ptr(its->m, its->cbaser & 0x0000fffffffff000ull, bytes);
    if (!q) return;
    uint32_t mask = (uint32_t)(bytes - 1);
    uint32_t rd = its->creadr & mask;
    for (int guard = 0; guard < 8192 && rd != (write & mask); guard++) {
        uint64_t w[4];
        for (int i = 0; i < 4; i++) w[i] = rd64(q + ((rd + (uint32_t)i * 8) & mask));
        do_cmd(its, w[0], w[1], w[2]);
        rd = (rd + 32) & mask;
    }
    its->creadr = rd;
}

/* A device's MSI: DeviceID and EventID, looked up and pended. */
static void deliver(HvIts *its, uint32_t device, uint32_t event) {
    struct HvItsDev *d = find_dev(its, device, 0);
    if (!d) return;
    int cpu = coll_cpu(its, d->collection);
    if (cpu < 0) return;
    for (int i = 0; i < d->nev; i++)
        if (d->ev[i].event == event) {
            pend_lpi(its, d->ev[i].lpi, cpu);
            return;
        }
}

void hv_its_signal(HvIts *its, uint32_t device, uint32_t event) {
    pthread_mutex_lock(&its->mu);
    deliver(its, device, event);
    pthread_mutex_unlock(&its->mu);
}

void hv_its_poll(HvIts *its) {
    pthread_mutex_lock(&its->mu);
    run_commands(its, its->cwriter);
    pthread_mutex_unlock(&its->mu);
}

static uint64_t its_read(void *opaque, uint64_t off, unsigned size) {
    HvIts *its = opaque;
    (void)size;
    switch ((uint32_t)off & 0xfffu) {
    case R_CTLR: return its->ctlr;
    case R_TYPER: return its->typer;
    case R_CBASER: return its->cbaser;
    case R_CREADR: return its->creadr;
    case R_CWRITER: return its->cwriter;
    default: return 0;
    }
}

static void its_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvIts *its = opaque;
    (void)size;
    switch ((uint32_t)off & 0xfffu) {
    case R_CTLR:
        its->ctlr = (uint32_t)v & 1;
        if (!(its->ctlr & 1)) its->creadr = 0;
        break;
    case R_CBASER: {
        int was = (int)(its->cbaser & 1);
        its->cbaser = v;
        if (!(v & 1) || !was) its->creadr = 0;
        break;
    }
    case R_CWRITER:
        its->cwriter = (uint32_t)v;
        run_commands(its, its->cwriter);
        break;
    case R_TRANSLATER: {
        uint32_t device = (uint32_t)(v >> 32), event = (uint32_t)(v & 0xffff);
        deliver(its, device, event);
        break;
    }
    default:
        if (its->m->log) fprintf(stderr, "its: write +%#llx = %#llx\n", (unsigned long long)off, (unsigned long long)v);
        break;
    }
}

HvIts *hv_its_create(HvMachine *m, HvGic *gic, uint64_t pa, uint64_t size, int (*rd_cpu)(uint32_t rd_base)) {
    HvIts *its = calloc(1, sizeof *its);
    if (!its) return NULL;
    its->m = m;
    its->gic = gic;
    its->rd_cpu = rd_cpu;
    pthread_mutex_init(&its->mu, NULL);
    for (int c = 0; c < HV_MAX_CPUS; c++) its->coll[c] = -1;
    its->typer = 1ull              /* physical LPIs */
                 | (7ull << 8)     /* ITT entry size 8 bytes */
                 | (7ull << 16)    /* 8-bit EventIDs: [23:16], so the opcode in [31:24] does not collide */
                 | (23ull << 19)   /* 24-bit DeviceIDs */
                 | ((uint64_t)(HV_ITS_LPI_COUNT - 1) << 24); /* last LPI INTID's offset */
    if (hv_machine_add_mmio(m, pa, size, its, its_read, its_write, "gicv3-its") != 0) {
        free(its);
        return NULL;
    }
    return its;
}
