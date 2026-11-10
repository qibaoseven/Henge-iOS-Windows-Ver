#include "pcie.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "its.h"

/* The bridge, as QEMU's pci-host-ecam-generic presents it.

   ECAM: offset = bus << 20 | dev << 15 | fn << 12 | reg. The window here is
   16 MB, so all 256 buses fit. A function that is not there reads all ones and
   swallows writes, which is what makes the OS stop enumerating.

   BAR assignment: a BAR with no address answers its size when the OS writes all
   ones (the classic probe), and takes whatever address it is given afterwards.
   Windows assigns every BAR before it touches a device, so this has to be exact. */

#define PCI_COMMAND 0x04
#define PCI_STATUS 0x06
#define CMD_MEMORY 0x2
#define CMD_INTX_DISABLE 0x400
#define STATUS_INTX 0x8
#define STATUS_CAPS 0x10

#define HDR_TYPE 0x0e
#define CAP_LIST 0x34
#define CAP_MSI 0x05
#define CAP_MSIX 0x11
#define CAP_PCIE 0x10

static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static uint16_t get16(const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return v;
}
static uint32_t get32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

void hv_pci_init(HvPciDev *d, uint16_t vendor, uint16_t device, uint32_t class_rev, uint16_t sub_vendor,
                 uint16_t sub_id) {
    memset(d->config, 0, sizeof d->config);
    memset(d->wmask, 0, sizeof d->wmask);
    put16(d->config + 0x00, vendor);
    put16(d->config + 0x02, device);
    put32(d->config + 0x08, class_rev);
    put16(d->config + 0x2c, sub_vendor);
    put16(d->config + 0x2e, sub_id);
    d->config[0x3d] = 1;                   /* INTA */
    put16(d->config + PCI_STATUS, STATUS_CAPS);
    put16(d->wmask + PCI_COMMAND, 0x0407); /* I/O, memory, bus master, INTx disable */
    d->wmask[0x0c] = 0xff;
    d->wmask[0x0d] = 0xff;
    d->wmask[0x3c] = 0xff;
}

void hv_pci_add_bar(HvPciDev *d, int bar, uint64_t size, int is64) {
    d->bar_size[bar] = size;
    d->bar64[bar] = (uint8_t)is64;
    put32(d->config + 0x10 + bar * 4, is64 ? 0x4 : 0x0); /* memory, 32/64-bit, not prefetchable */
    put32(d->wmask + 0x10 + bar * 4, (uint32_t)~(size - 1) & ~0xfu);
    if (is64) put32(d->wmask + 0x14 + bar * 4, (uint32_t)(~(size - 1) >> 32));
}

/* The capability list: standard header capabilities below 0x100, PCIe extended
   ones at or above it. Both start at the head the header points to. */
/* Appends a capability and returns where it went. The list is a chain of
   {id, next, ...}; the new one goes at the first free slot and the last node
   before it is pointed at it, so walking the chain reaches every capability. */
/* Append a capability at the end of the chain. `d->cap_next` is where the next
   one goes, so adding two in a row cannot land on top of each other. */
static uint8_t *cap_add(HvPciDev *d, uint8_t id, uint8_t len) {
    if (!d->config[CAP_LIST]) {
        d->config[CAP_LIST] = 0x40;
        d->wmask[CAP_LIST] = 0xff;
    }
    if (d->cap_next < 0x40) d->cap_next = 0x40; /* nothing yet, or a device that never set it */
    if (d->cap_next + len > 0x100) return NULL;
    uint8_t at = d->cap_next;
    /* The previous node must point here; the first one is pointed at by 0x34. */
    if (at != 0x40) {
        uint8_t p = 0x40;
        while (d->config[p + 1] && p < 0x100) p = d->config[p + 1];
        d->config[p + 1] = at;
    }
    d->config[at] = id;
    d->config[at + 1] = 0;
    d->config[at + 2] = len;
    d->cap_next = (uint8_t)(at + len);
    put16(d->config + PCI_STATUS, get16(d->config + PCI_STATUS) | STATUS_CAPS);
    return d->config + at;
}

int hv_pci_add_msix(HvPciDev *d, int n, int bar, uint32_t table_off, uint32_t pba_off) {
    uint8_t *c = cap_add(d, CAP_MSIX, 12);
    if (!c) return -1;
    int off = (int)(c - d->config);
    /* Control: bits 10:0 hold table size - 1; bits 14 (function mask) and 15
       (enable) start clear -- a driver turns MSI-X on itself. */
    put16(c + 2, (uint16_t)(n - 1));
    /* Table and PBA descriptors: BAR index in 3 bits, then the offset >> 3. */
    put32(c + 4, (uint32_t)(bar & 7) | ((table_off >> 3) & ~7u));
    put32(c + 8, (uint32_t)(bar & 7) | ((pba_off >> 3) & ~7u));
    d->msix_vectors = n;
    /* Bits 10:0 (size), 14 (function mask), 15 (enable) are writable. */
    d->wmask[off + 2] = 0xfb;
    d->wmask[off + 3] = 0xc7;
    return off;
}

int hv_pci_add_pcie_cap(HvPciDev *d) {
    uint8_t *c = cap_add(d, CAP_PCIE, 0x3c); /* 60 bytes: device, link, slot, root */
    if (!c) return -1;
    int off = (int)(c - d->config);
    /* PCI Express Capabilities: version 2, endpoint (type 0) or root port. */
    c[2] = 2;
    c[3] = 0 /* endpoint */;
    /* Device Capabilities (at +4): everything the OS probes is reported off. */
    put32(c + 4, 0x00000000);
    /* Device Status/Control at +8/+0xa. */
    put16(c + 8, 0);
    put16(c + 0xa, 0);
    /* Link Capabilities/Control/Status at +0xc/+0x10/+0x12: no training. */
    put32(c + 0xc, 0x00000000);
    put16(c + 0x10, 0);
    put16(c + 0x12, 1 /* link is up (speed 1, width x1) */);
    /* Root port registers (they exist in every PCIe capability, ignored by
       endpoints): empty. */
    d->wmask[off + 0xa] = 0xff; /* device control */
    d->wmask[off + 0xb] = 0x07;
    return off;
}

static void update_line(HvPcie *p, int line) {
    hv_gicv3_set_irq(p->gic, p->intx_base + (line & 3), p->line_count[line] > 0);
}

void hv_pci_set_intx(HvPciDev *d, int level) {
    HvPcie *p = d->bus;
    if (!p) return;
    uint16_t cmd = get16(d->config + PCI_COMMAND);
    int eff = level && !(cmd & CMD_INTX_DISABLE);
    uint16_t status = get16(d->config + PCI_STATUS);
    put16(d->config + PCI_STATUS, level ? status | STATUS_INTX : status & ~STATUS_INTX);
    if (eff == d->intx) return;
    d->intx = eff;
    /* Slot's INTx is interrupt-base + (slot % 4): QEMU's gpex swizzle. */
    int line = d->devfn;
    p->line_count[line] += eff ? 1 : -1;
    update_line(p, line);
}

/* An interrupt from a device: an MSI through the ITS if MSI-X is on, otherwise
   the legacy line. Windows' virtio drivers never take the line. */
void hv_pci_interrupt(HvPciDev *d) {
    HvPcie *p = d->bus;
    if (!p) return;
    if (d->msix_vectors && p->its) {
        /* MSI-X is only usable once the guest has enabled it and cleared the
           function mask; until then a driver that asked for MSI has not got it. */
        uint8_t at = d->config[CAP_LIST];
        for (int guard = 0; at >= 0x40 && at < 0x100 && guard < 32; guard++) {
            if (d->config[at] == CAP_MSIX) {
                uint16_t ctl = get16(d->config + at + 2);
                if ((ctl & 0x8000) && !(ctl & 0x4000) && d->msi) {
                    d->msi(d->opaque, d->msix_current_vector);
                    return;
                }
                break;
            }
            at = d->config[at + 1];
        }
    }
    hv_pci_set_intx(d, 1);
    hv_pci_set_intx(d, 0); /* a level line a legacy driver acknowledges by EOIR */
}

static uint64_t ecam_read(void *opaque, uint64_t off, unsigned size) {
    HvPcie *p = opaque;
    uint32_t bus = (uint32_t)(off >> 20), slot = (off >> 15) & 31, fn = (off >> 12) & 7, reg = off & 0xfff;
    int devfn = (int)((bus << 8) | (slot << 3) | fn);
    HvPciDev *d = devfn < HV_PCI_MAX_DEV ? p->dev[devfn] : NULL;
    if (!d) return size == 8 ? ~0ull : (1ull << (size * 8)) - 1;
    uint64_t v = 0;
    memcpy(&v, d->config + reg, size > 4096 - reg ? 4096 - reg : size);
    if (getenv("HV_PCI_LOG") && bus == 0 && slot == (uint32_t)atoi(getenv("HV_PCI_LOG")))
        fprintf(stderr, "pci %u:%u.%u: read %03x (%u) = %llx\n", bus, slot, fn, reg, size, (unsigned long long)v);
    return v;
}

static void ecam_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvPcie *p = opaque;
    uint32_t bus = (uint32_t)(off >> 20), slot = (off >> 15) & 31, fn = (off >> 12) & 7, reg = off & 0xfff;
    int devfn = (int)((bus << 8) | (slot << 3) | fn);
    HvPciDev *d = devfn < HV_PCI_MAX_DEV ? p->dev[devfn] : NULL;
    if (!d) return;
    uint16_t old_cmd = get16(d->config + PCI_COMMAND);
    for (unsigned i = 0; i < size && reg + i < 256; i++) {
        uint8_t w = d->wmask[reg + i], b = (uint8_t)(v >> (i * 8));
        d->config[reg + i] = (uint8_t)((d->config[reg + i] & ~w) | (b & w));
    }
    if ((old_cmd ^ get16(d->config + PCI_COMMAND)) & CMD_INTX_DISABLE) {
        int level = (get16(d->config + PCI_STATUS) & STATUS_INTX) != 0;
        hv_pci_set_intx(d, level);
    }
}

static HvPciDev *bar_lookup(HvPcie *p, uint64_t pa, int *bar, uint64_t *off) {
    for (int i = 0; i < HV_PCI_MAX_DEV; i++) {
        HvPciDev *d = p->dev[i];
        if (!d || !(get16(d->config + PCI_COMMAND) & CMD_MEMORY)) continue;
        for (int b = 0; b < 6; b++) {
            if (!d->bar_size[b]) continue;
            uint64_t base = hv_pci_bar_addr(d, b);
            if (base && pa - base < d->bar_size[b]) {
                *bar = b;
                *off = pa - base;
                return d;
            }
        }
    }
    return NULL;
}

static uint64_t window_read(void *opaque, uint64_t off, unsigned size) {
    HvPcie *p = opaque;
    int bar;
    uint64_t boff;
    HvPciDev *d = bar_lookup(p, p->mmio_base + off, &bar, &boff);
    if (!d || !d->bar_read) return ~0ull;
    return d->bar_read(d, bar, boff, size);
}

static void window_write(void *opaque, uint64_t off, uint64_t v, unsigned size) {
    HvPcie *p = opaque;
    int bar;
    uint64_t boff;
    HvPciDev *d = bar_lookup(p, p->mmio_base + off, &bar, &boff);
    if (d && d->bar_write) d->bar_write(d, bar, boff, v, size);
}

uint64_t hv_pci_bar_addr(HvPciDev *d, int bar) {
    uint64_t lo = get32(d->config + 0x10 + bar * 4) & ~0xfu;
    if (d->bar64[bar]) lo |= (uint64_t)get32(d->config + 0x14 + bar * 4) << 32;
    return lo;
}

int hv_pcie_attach(HvPcie *p, HvPciDev *d) {
    /* Device 1 onward: device 0 function 0 is the host bridge, as QEMU has it,
       and a function 1 with no function 0 before it is not a shape an OS scans. */
    for (int s = 1; s < HV_PCI_SLOTS; s++) {
        for (int f = 0; f < HV_PCI_FUNCS; f++) {
            int devfn = (s << 3) | f;
            if (p->dev[devfn]) continue;
            p->dev[devfn] = d;
            d->bus = p;
            d->devfn = devfn;
            return devfn;
        }
    }
    return -1;
}

void hv_pcie_set_its(HvPcie *p, void *its) { p->its = its; }

HvPcie *hv_pcie_create(HvMachine *m, HvGic *gic, uint64_t ecam_pa, uint64_t ecam_size, uint64_t mmio_pa,
                       uint64_t mmio_size, int intx_intid) {
    HvPcie *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->m = m;
    p->gic = gic;
    p->intx_base = intx_intid;
    p->mmio_base = mmio_pa;
    p->mmio_size = mmio_size;
    p->ecam_pa = ecam_pa;
    p->ecam_size = ecam_size;
    /* The host bridge: QEMU's i440FX-style PCIe root, class 0x060000, type 1. */
    hv_pci_init(&p->bridge, 0x1b36, 0x0008, 0x06000000, 0x1af4, 0x1100);
    p->bridge.config[HDR_TYPE] = 1; /* PCI-to-PCI bridge header */
    p->bridge.config[0x18] = 0;     /* primary bus */
    p->bridge.config[0x19] = 0;     /* secondary bus */
    p->bridge.config[0x1a] = (uint8_t)(HV_PCI_MAX_BUS - 1); /* subordinate bus */
    p->dev[0] = &p->bridge;
    p->bridge.bus = p;
    hv_machine_add_mmio(m, ecam_pa, ecam_size, p, ecam_read, ecam_write, "pcie-ecam");
    hv_machine_add_mmio(m, mmio_pa, mmio_size, p, window_read, window_write, "pcie-mmio");
    return p;
}
