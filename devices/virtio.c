#include "virtio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "its.h"

/* Layout of the one BAR (BAR 4, 16 KB), as QEMU's virtio-pci lays it out. */
#define BAR 4
#define COMMON 0x0000
#define ISR 0x1000
#define DEVICE 0x2000
#define NOTIFY 0x3000
#define NOTIFY_MULT 4
#define MSIX_TABLE 0x4000 /* above the notify area: 16 bytes per vector */
#define MSIX_PBA 0x8000

#define DESC_F_NEXT 1
#define DESC_F_WRITE 2
#define DESC_F_INDIRECT 4

static void add_cap(HvVirtio *v, uint8_t at, uint8_t next, uint8_t type, uint32_t off, uint32_t len) {
    uint8_t *c = v->pci.config + at;
    c[0] = 0x09;                    /* vendor-specific */
    c[1] = next;
    c[2] = type == 2 ? 20 : 16;
    c[3] = type;
    c[4] = BAR;
    memcpy(c + 8, &off, 4);
    memcpy(c + 12, &len, 4);
    if (type == 2) {
        uint32_t mult = NOTIFY_MULT;
        memcpy(c + 16, &mult, 4);
    }
}

static void reset(HvVirtio *v) {
    v->driver_features = 0;
    v->dfsel = v->gfsel = 0;
    v->status = 0;
    v->isr = 0;
    v->qsel = 0;
    for (int i = 0; i < v->nq; i++) v->q[i] = (HvVirtq){.size = HV_VIRTQ_MAX_SIZE};
    hv_pci_set_intx(&v->pci, 0);
}

static uint64_t common_read(HvVirtio *v, uint64_t off) {
    HvVirtq *q = v->qsel < v->nq ? &v->q[v->qsel] : NULL;
    switch (off) {
    case 0x00: return v->dfsel;
    case 0x04: return v->dfsel < 2 ? (uint32_t)(v->features >> (32 * v->dfsel)) : 0;
    case 0x08: return v->gfsel;
    case 0x0c: return v->gfsel < 2 ? (uint32_t)(v->driver_features >> (32 * v->gfsel)) : 0;
    case 0x10: return (uint16_t)(v->msix_vector < 0 ? 0xffff : v->msix_vector); /* config_msix_vector */
    case 0x12: return v->nq;
    case 0x14: return v->status;
    case 0x15: return v->config_gen;
    case 0x16: return v->qsel;
    case 0x18: return q ? q->size : 0;
    case 0x1a: return 0xffff;
    case 0x1c: return q ? q->enabled : 0;
    case 0x1e: return v->qsel;                 /* queue_notify_off = queue index */
    case 0x20: return q ? (uint32_t)q->desc : 0;
    case 0x24: return q ? q->desc >> 32 : 0;
    case 0x28: return q ? (uint32_t)q->avail : 0;
    case 0x2c: return q ? q->avail >> 32 : 0;
    case 0x30: return q ? (uint32_t)q->used : 0;
    case 0x34: return q ? q->used >> 32 : 0;
    default: return 0;
    }
}

static void set64(uint64_t *f, uint64_t off, uint64_t v, unsigned size) {
    if (size == 8) *f = v;
    else if (off & 4) *f = (*f & 0xffffffffull) | v << 32;
    else *f = (*f & ~0xffffffffull) | (uint32_t)v;
}

static void common_write(HvVirtio *v, uint64_t off, uint64_t val, unsigned size) {
    HvVirtq *q = v->qsel < v->nq ? &v->q[v->qsel] : NULL;
    switch (off) {
    case 0x00: v->dfsel = (uint32_t)val; break;
    case 0x08: v->gfsel = (uint32_t)val; break;
    case 0x0c:
        if (v->gfsel < 2) {
            uint64_t mask = 0xffffffffull << (32 * v->gfsel);
            v->driver_features = (v->driver_features & ~mask) | (((uint64_t)(uint32_t)val << (32 * v->gfsel)) & v->features);
        }
        break;
    case 0x14:
        if ((uint8_t)val == 0) reset(v);
        else v->status = (uint8_t)val;
        break;
    case 0x16: v->qsel = (uint16_t)val; break;
    case 0x18:
        if (q && val && val <= HV_VIRTQ_MAX_SIZE && !(val & (val - 1))) q->size = (uint16_t)val;
        else if (q) /* the driver would index its rings by a size we do not use: say so */
            fprintf(stderr, "virtio %04x: queue %u size %llu refused (max %d)\n", v->pci.config[2] | v->pci.config[3] << 8,
                    v->qsel, (unsigned long long)val, HV_VIRTQ_MAX_SIZE);
        if (getenv("HV_VIRTIO_CFG_LOG"))
            fprintf(stderr, "virtio %04x: queue %u size %llu\n", v->pci.config[2] | v->pci.config[3] << 8, v->qsel, (unsigned long long)val);
        break;
    case 0x1c: if (q) q->enabled = val & 1; break;
    case 0x20: case 0x24: if (q) set64(&q->desc, off, val, size); break;
    case 0x28: case 0x2c: if (q) set64(&q->avail, off, val, size); break;
    case 0x30: case 0x34: if (q) set64(&q->used, off, val, size); break;
    default: break;
    }
}

static uint64_t bar_read(HvPciDev *d, int bar, uint64_t off, unsigned size) {
    HvVirtio *v = d->opaque;
    if (bar != BAR) return ~0ull;
    if (off < ISR) return common_read(v, off);
    if (off < DEVICE) { /* ISR: reading clears it and drops the line */
        uint8_t isr = v->isr;
        v->isr = 0;
        return isr;
    }
    if (off >= MSIX_TABLE && off < MSIX_TABLE + 0x1000) { /* the MSI-X table */
        uint32_t o = (uint32_t)(off - MSIX_TABLE);
        int vec = (int)(o / 16);
        uint32_t w = o % 16;
        if (vec >= v->nmsix) return 0;
        uint64_t r = 0;
        if (w < 8) r = v->msix[vec].addr >> (w * 8);
        else if (w < 12) r = v->msix[vec].data >> ((w - 8) * 8);
        else r = v->msix[vec].ctrl >> ((w - 12) * 8);
        return r & ((size >= 8) ? ~0ull : (1ull << (size * 8)) - 1);
    }
    if (off >= MSIX_PBA && off < MSIX_PBA + 0x1000) return 0; /* no pending bits: an MSI is delivered at once */
    if (off < NOTIFY) {
        uint64_t r = 0;
        uint32_t o = (uint32_t)(off - DEVICE);
        if (o < v->config_len) memcpy(&r, v->config + o, size > v->config_len - o ? v->config_len - o : size);
        static int cfg_log = -1;
        if (cfg_log < 0) cfg_log = getenv("HV_VIRTIO_CFG_LOG") != NULL;
        if (cfg_log) /* debugging: which device config fields the driver reads */
            fprintf(stderr, "virtio %04x:%04x config read +%u (%u) = %llx\n", v->pci.config[0] | v->pci.config[1] << 8,
                    v->pci.config[2] | v->pci.config[3] << 8, o, size, (unsigned long long)r);
        return r;
    }
    return 0;
}

static void bar_write(HvPciDev *d, int bar, uint64_t off, uint64_t val, unsigned size) {
    HvVirtio *v = d->opaque;
    if (bar != BAR) return;
    if (off >= MSIX_TABLE && off < MSIX_TABLE + 0x1000) {
        uint32_t o = (uint32_t)(off - MSIX_TABLE);
        int vec = (int)(o / 16);
        uint32_t w = o % 16;
        if (vec >= v->nmsix) return;
        /* An entry is 0..7 address, 8..11 data, 12..15 control. The shift is
           within the field's own width: the address is 64 bits, the other two
           32, and shifting a 32-bit control word by 32 leaves nothing. */
        uint64_t m = (size >= 8) ? ~0ull : ((1ull << (size * 8)) - 1);
        if (w < 8) {
            uint64_t sh = (uint64_t)(w & 7) * 8;
            v->msix[vec].addr = (v->msix[vec].addr & ~(m << sh)) | ((val & m) << sh);
        } else if (w < 12) {
            uint64_t sh = (uint64_t)(w - 8) * 8;
            v->msix[vec].data = (uint32_t)((v->msix[vec].data & ~(m << sh)) | ((val & m) << sh));
        } else {
            uint64_t sh = (uint64_t)(w - 12) * 8;
            v->msix[vec].ctrl = (uint32_t)((v->msix[vec].ctrl & ~(m << sh)) | ((val & m) << sh));
        }
        if (v->m->log && w == 0)
            fprintf(stderr, "virtio: MSI-X vector %d addr %llx data %x (ctrl %u)\n", vec,
                    (unsigned long long)v->msix[vec].addr, v->msix[vec].data, v->msix[vec].ctrl);
        return;
    }
    if (off < ISR) {
        common_write(v, off, val, size);
    } else if (off >= DEVICE && off < NOTIFY) {
        if (v->config_write) v->config_write(v, (uint32_t)(off - DEVICE), val, size);
    } else if (off >= NOTIFY) {
        int q = (int)((off - NOTIFY) / NOTIFY_MULT);
        if (q < v->nq && v->q[q].enabled && (v->status & 4) && v->notify) v->notify(v, q); /* DRIVER_OK */
    }
}

void hv_virtio_init(HvVirtio *v, HvMachine *m, int device_id, uint32_t class_code, int nq, uint64_t features) {
    memset(v, 0, sizeof *v);
    v->m = m;
    v->nq = (uint16_t)nq;
    v->features = features | HV_VIRTIO_F_VERSION_1;
    hv_pci_init(&v->pci, 0x1af4, (uint16_t)(0x1040 + device_id), class_code << 8 | 1, 0x1af4, (uint16_t)device_id);
    v->pci.opaque = v;
    v->pci.bar_read = bar_read;
    v->pci.bar_write = bar_write;
    hv_pci_add_bar(&v->pci, BAR, 0x10000, 1);
    v->pci.config[0x06] |= 0x10;                 /* capability list */
    v->pci.config[0x34] = 0x40;
    add_cap(v, 0x40, 0x50, 1, COMMON, 0x38);
    add_cap(v, 0x50, 0x60, 3, ISR, 1);
    add_cap(v, 0x60, 0x70, 4, DEVICE, 0x100);
    add_cap(v, 0x70, 0x00, 2, NOTIFY, NOTIFY_MULT * HV_VIRTQ_MAX);
    /* MSI-X, with one vector for config changes and one per queue: without it
       Windows' virtio drivers refuse the device (they ask for MSI and stop). */
    v->nmsix = nq + 1;
    v->msix_vector = -1;
    hv_pci_add_msix(&v->pci, v->nmsix, BAR, MSIX_TABLE, MSIX_PBA);
    reset(v);
}

/* The table entry's message goes to the ITS as an MSI. A masked vector, or one
   with no message, gets nothing -- a driver that asked for MSI has said it will
   not look at the line. */
static void msix_raise(HvVirtio *v, int vector) {
    if (vector < 0 || vector >= v->nmsix) return;
    struct HvMsixEntry *e = &v->msix[vector];
    if (e->ctrl & 1) return;
    if (!e->addr) return;
    v->pci.msix_current_vector = vector;
    HvPcie *bus = v->pci.bus;
    /* The ITS names a device by its requester ID: what the device wrote into the
       message is only the address and the EventID. */
    if (bus && bus->its) hv_its_signal((HvIts *)bus->its, (uint32_t)v->pci.devfn, e->data);
}

void hv_virtio_interrupt(HvVirtio *v) {
    v->isr |= 1;
    msix_raise(v, v->msix_vector >= 0 ? v->msix_vector : 1);
}

void hv_virtio_config_changed(HvVirtio *v) {
    v->config_gen++;
    v->isr |= 2;
    msix_raise(v, 0);
}

/* A queue's own interrupt: virtio's per-queue MSI-X, which is what a driver
   with several queues expects (queue q -> vector q + 1). */
void hv_virtio_queue_interrupt(HvVirtio *v, int q) { msix_raise(v, q + 1); }

int hv_virtq_pop(HvVirtio *v, int qi, HvVirtqElem *e) {
    HvVirtq *q = &v->q[qi];
    uint16_t *avail = hv_phys_ptr(v->m, q->avail, 4 + 2u * q->size);
    if (!avail) return 0; /* not set up: nothing taken (-1 means an element was taken but is broken) */
    uint16_t idx = __atomic_load_n(&avail[1], __ATOMIC_ACQUIRE);
    if (q->last_avail == idx) return 0;
    /* The guest writes the ring entry (and the descriptors) before the index,
       from another core: the acquire above keeps these reads after it, or a
       stale entry (the driver fills unused ones with 0xffff) comes back. */
    uint16_t head = __atomic_load_n(&avail[2 + q->last_avail % q->size], __ATOMIC_RELAXED);
    if (head >= q->size) {
        /* Not written yet as far as this core can see (the driver keeps unused
           entries at 0xffff). Taking it would complete a request that does not
           exist (the guest panics "Adding invalid index 65535") or lose the
           real one: leave it for the next notify or poll. */
        static int said;
        if (said++ < 10)
            fprintf(stderr, "virtio %04x: avail entry %u not ready (head %u, idx %u); retrying later\n",
                    v->pci.config[2] | v->pci.config[3] << 8, q->last_avail, head, idx);
        return 0;
    }
    q->last_avail++;
    if (q->out[head]) {
        static int said;
        if (said++ < 10)
            fprintf(stderr, "virtio %04x q%d: head %u taken again while still outstanding\n",
                    v->pci.config[2] | v->pci.config[3] << 8, qi, head);
    }
    q->out[head] = 1;
    e->head = head;
    e->nseg = 0;
    uint64_t table = q->desc;
    uint32_t count = q->size;
    uint16_t i = head;
    for (uint32_t steps = 0; steps < HV_VIRTQ_MAX_SEGS; steps++) {
        if (i >= count) return -1;
        uint8_t *d = hv_phys_ptr(v->m, table + 16u * i, 16);
        if (!d) return -1;
        uint64_t addr;
        uint32_t len;
        uint16_t flags, next;
        memcpy(&addr, d, 8);
        memcpy(&len, d + 8, 4);
        memcpy(&flags, d + 12, 2);
        memcpy(&next, d + 14, 2);
        if (flags & DESC_F_INDIRECT) { /* not offered, but harmless to follow */
            table = addr;
            count = len / 16;
            i = 0;
            continue;
        }
        if (e->nseg == HV_VIRTQ_MAX_SEGS) return -1;
        e->seg[e->nseg++] = (HvVirtqSeg){addr, len, (flags & DESC_F_WRITE) != 0};
        if (!(flags & DESC_F_NEXT)) return 1;
        i = next;
    }
    return -1;
}

void hv_virtq_push(HvVirtio *v, int qi, const HvVirtqElem *e, uint32_t written) {
    HvVirtq *q = &v->q[qi];
    if (e->head >= q->size || !q->out[e->head]) {
        /* Not something the driver gave us (or given back already): pushing it
           would corrupt its free list. */
        static int said;
        if (said++ < 10)
            fprintf(stderr, "virtio %04x q%d: completion of head %u that is not outstanding (dropped)\n",
                    v->pci.config[2] | v->pci.config[3] << 8, qi, e->head);
        return;
    }
    q->out[e->head] = 0;
    uint8_t *used = hv_phys_ptr(v->m, q->used, 4 + 8u * q->size);
    if (!used) return;
    uint16_t idx;
    memcpy(&idx, used + 2, 2);
    uint32_t id = e->head;
    memcpy(used + 4 + 8u * (idx % q->size), &id, 4);
    memcpy(used + 8 + 8u * (idx % q->size), &written, 4);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    idx++;
    memcpy(used + 2, &idx, 2);
}

static uint32_t copy(HvVirtio *v, const HvVirtqElem *e, int writable, uint32_t off, uint8_t *buf, uint32_t len) {
    uint32_t done = 0;
    for (int s = 0; s < e->nseg && done < len; s++) {
        const HvVirtqSeg *g = &e->seg[s];
        if (g->write != writable) continue;
        if (off >= g->len) {
            off -= g->len;
            continue;
        }
        uint32_t n = g->len - off;
        if (n > len - done) n = len - done;
        uint8_t *p = hv_phys_ptr(v->m, g->addr + off, n);
        if (!p) break;
        if (writable) memcpy(p, buf + done, n);
        else memcpy(buf + done, p, n);
        done += n;
        off = 0;
    }
    return done;
}

uint32_t hv_virtq_read(HvVirtio *v, const HvVirtqElem *e, uint32_t off, void *buf, uint32_t len) {
    return copy(v, e, 0, off, buf, len);
}

uint32_t hv_virtq_write(HvVirtio *v, const HvVirtqElem *e, uint32_t off, const void *buf, uint32_t len) {
    return copy(v, e, 1, off, (uint8_t *)buf, len);
}

uint32_t hv_virtq_writable(const HvVirtqElem *e) {
    uint32_t n = 0;
    for (int s = 0; s < e->nseg; s++)
        if (e->seg[s].write) n += e->seg[s].len;
    return n;
}
