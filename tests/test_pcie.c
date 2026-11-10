/* PCIe and virtio MSI-X: the two things Windows will not use a device without.

   The bridge has to answer over every bus, report its extended capabilities,
   let the OS take a BAR apart with all-ones and put it back elsewhere; and a
   virtio device has to turn a queue's interrupt into an MSI that the ITS
   delivers as an LPI on the right CPU. Each is driven here the way a driver
   would, through the machine's MMIO map. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "its.h"
#include "machine.h"
#include "pcie.h"
#include "virtio.h"
#include "virt.h"

static int *cnt; /* {checks, fails}: on the heap, so a stray write into BSS cannot hide a result */
#define checks (cnt[0])
#define fails (cnt[1])
#define CHECK(c, ...)                                   \
    do {                                                \
        checks++;                                       \
        if (!(c)) {                                     \
            fails++;                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static uint64_t rd(HvMachine *m, uint64_t pa, unsigned size) {
    HvMmio *x = hv_mmio_lookup(m, pa);
    return x ? x->read(x->opaque, pa - x->pa, size) : ~0ull;
}
static void wr(HvMachine *m, uint64_t pa, uint64_t v, unsigned size) {
    HvMmio *x = hv_mmio_lookup(m, pa);
    if (x) x->write(x->opaque, pa - x->pa, v, size);
}
/* ECAM address of a register. */
static uint64_t cfg(uint32_t bus, uint32_t dev, uint32_t fn, uint32_t reg) {
    return VIRT_PCIE_ECAM + ((uint64_t)bus << 20) + (dev << 15) + (fn << 12) + reg;
}

int main(void) {
    cnt = calloc(2, sizeof *cnt);
    HvMachine *m = calloc(1, sizeof *m);
    if (hv_machine_init(m, 8u << 20) != 0) return 1;
    HvCpu *cpus = calloc(4, sizeof *cpus);
    HvVirt v;
    if (hv_virt_create(&v, m, cpus, 4, 256u << 20, NULL, NULL) != 0) {
        printf("cannot build the machine\n");
        return 1;
    }
    hv_virt_reset_cpu(&v); /* pc_state is set there: the GIC hands interrupts to it */
    HvPcie *bus = NULL;
    for (int i = 0; i < m->nmmio; i++)
        if (!strcmp(m->mmio[i].name, "pcie-ecam")) bus = m->mmio[i].opaque;
    CHECK(bus != NULL, "the bridge exists");
    hv_pcie_set_its(bus, v.its);
    CHECK(bus->its == v.its, "the bridge has the ITS");

    /* --- the host bridge is where an OS starts: bus 0, dev 0, fn 0 --- */
    CHECK(rd(m, cfg(0, 0, 0, 0x00), 4) == 0x00081b36u, "host bridge vendor/device");
    CHECK(rd(m, cfg(0, 0, 0, 0x08), 4) == 0x06000000u, "class 0x060000");
    CHECK(rd(m, cfg(0, 0, 0, 0x0e), 1) == 1, "bridge header type 1");
    CHECK(rd(m, cfg(0, 0, 0, 0x19), 1) == 0, "secondary bus 0");
    CHECK(rd(m, cfg(0, 0, 0, 0x1a), 1) == HV_PCI_MAX_BUS - 1, "subordinate bus covers the range");

    /* --- the whole bus range answers, and an empty one reads all ones --- */
    CHECK((uint32_t)rd(m, cfg(1, 0, 0, 0x00), 4) == 0xffffffffu, "bus 1 has nothing: all ones");
    CHECK((uint32_t)rd(m, cfg(HV_PCI_MAX_BUS - 1, 31, 7, 0x00), 4) == 0xffffffffu, "the last bus and function read all ones");
    wr(m, cfg(HV_PCI_MAX_BUS - 1, 31, 7, 0x00), 0x1234, 4); /* and swallow writes */
    CHECK((uint32_t)rd(m, cfg(HV_PCI_MAX_BUS - 1, 31, 7, 0x00), 4) == 0xffffffffu, "a write to nothing changes nothing");
    /* --- a virtio-blk device --- */
    HvVirtio blk;
    memset(&blk, 0, sizeof blk);
    hv_virtio_init(&blk, m, 2 /* block */, 0x010000, 1, 0);
    int devfn = hv_pcie_attach(bus, &blk.pci);
    CHECK(devfn >= 0, "attached at %d", devfn);
    uint32_t dev = (uint32_t)(devfn >> 3), fn = (uint32_t)(devfn & 7);
    CHECK(rd(m, cfg(0, dev, fn, 0x00), 4) == 0x10421af4u, "VEN_1AF4 & DEV_1042, the modern block id");
    /* Extended capabilities: status bit 4, and a list the OS can walk. */
    CHECK((rd(m, cfg(0, dev, fn, 0x06), 2) & 0x10) != 0, "capabilities flag");
    uint8_t cap = (uint8_t)rd(m, cfg(0, dev, fn, 0x34), 1);
    int saw_msix = 0, saw_pcie = 0;
    for (int guard = 0; cap >= 0x40 && cap < 0x100 && guard < 16; guard++) {
        uint8_t id = (uint8_t)rd(m, cfg(0, dev, fn, cap), 1);
        if (id == 0x11) {
            saw_msix = 1;
            uint16_t ctl = (uint16_t)rd(m, cfg(0, dev, fn, cap + 2), 2);
            CHECK((ctl & 0x7ff) == 1, "MSI-X table size 2 (1 + 1 queue), got %u", ctl & 0x7ff);
            CHECK((ctl & 0x8000) == 0, "MSI-X starts disabled");
        }
        if (id == 0x10) saw_pcie = 1;
        cap = (uint8_t)rd(m, cfg(0, dev, fn, cap + 1), 1);
    }
    CHECK(saw_msix, "the device exposes MSI-X");
    hv_pci_add_pcie_cap(&blk.pci);
    cap = (uint8_t)rd(m, cfg(0, dev, fn, 0x34), 1);
    for (int guard = 0; cap >= 0x40 && cap < 0x100 && guard < 16; guard++) {
        if ((uint8_t)rd(m, cfg(0, dev, fn, cap), 1) == 0x10) saw_pcie = 1;
        cap = (uint8_t)rd(m, cfg(0, dev, fn, cap + 1), 1);
    }
    CHECK(saw_pcie, "the device exposes a PCIe capability");
    /* --- BAR assignment: all ones gives the size, then the address sticks --- */
    uint64_t baroff = cfg(0, dev, fn, 0x10 + 4 * 4); /* BAR 4 */
    wr(m, baroff, 0xffffffffu, 4);
    uint32_t probed = (uint32_t)rd(m, baroff, 4);
    CHECK((probed & ~0xfu) == 0xffff0000u, "BAR 4 probes 64 KB, got %#x", probed);
    wr(m, baroff, 0x10000000u, 4);                   /* the OS puts it at the window */
    CHECK(hv_pci_bar_addr(&blk.pci, 4) == 0x10000000u, "BAR 4 moved to 0x10000000");
    /* Memory space off: the window does not answer. On: it does. */
    wr(m, cfg(0, dev, fn, 0x04), 0x0, 2);
    CHECK((uint32_t)rd(m, VIRT_PCIE_MMIO + 0x0, 4) == 0xffffffffu, "BAR is closed until the device enables memory");
    wr(m, cfg(0, dev, fn, 0x04), 0x6, 2); /* memory space + bus master */
    CHECK((uint32_t)rd(m, VIRT_PCIE_MMIO + 0x0, 4) != 0xffffffffu, "the window answers once memory is enabled");
    {
    /* --- MSI-X: the driver writes a message, then a queue interrupt sends it --- */
        /* The guest's MSI-X table entry: address = GITS_TRANSLATER, data = the
           EventID. The ITS's translater is at its window's +0x40. */
        uint64_t translater = VIRT_GIC_ITS + 0x40;
        /* Enable the ITS and give it a command queue, then MAPD/MAPC/MAPTI. */
        uint8_t *q = calloc(1, 0x1000);
        uint64_t qpa = VIRT_GUEST_RAM + (8u << 20); /* inside guest RAM, unused by anything else */
        uint8_t *host = hv_phys_ptr(m, qpa, 0x1000);
        CHECK(host != NULL, "queue memory");
        (void)q;
        /* CBASER: base in [51:12], size - 1 in [7:0] (one 4 KB page), valid in [63]. */
        wr(m, VIRT_GIC_ITS + 0x80, (qpa & 0x0000fffffffff000ull) | 0x0ull | (1ull << 63), 8);
        wr(m, VIRT_GIC_ITS + 0x00, 1, 4);                                            /* CTLR enable */
        /* MAPC: collection 0 -> redistributor of CPU 0. MAPD: device 0x100 -> coll 0.
           MAPTI: device 0x100, event 1 -> LPI 8192. */
        uint64_t *cmd = (uint64_t *)host;
        /* Each command is four 64-bit words. Command 0: MAPC, with the
           collection in word0[15:0] and the redistributor's base in word0[63:32]. */
        cmd[0] = (0x09ull << 24) | (VIRT_GIC_REDIST << 32) | 0ull;
        /* Command 1: MAPD. word0 = DeviceID[63:32] | opcode[31:24] | collection[10:8]. */
        cmd[4] = ((uint64_t)(dev << 3 | fn) << 32) | (0x08ull << 24) | (0ull << 8);
        /* Command 2: MAPTI. word0 = DeviceID[63:32] | opcode[31:24] | event[31:16] | LPI[15:0]. */
        cmd[8] = ((uint64_t)(dev << 3 | fn) << 32) | (0x0bull << 24) | (1ull << 16) | HV_ITS_LPI_BASE;
        wr(m, VIRT_GIC_ITS + 0x98, 3 * 32, 4);                          /* CWRITER: three commands */

        /* The driver's MSI-X table entry for vector 1 (queue 0 -> vector 1). */
        uint64_t entry = VIRT_PCIE_MMIO + 0x4000 + 16 * 1;
        wr(m, entry + 0, translater, 8);       /* message address */
        wr(m, entry + 8, 1, 4);                /* message data = EventID 1 */
        /* Enable MSI-X on the device. */
        uint8_t at = (uint8_t)rd(m, cfg(0, dev, fn, 0x34), 1);
        int found = 0;
        for (int guard = 0; at >= 0x40 && at < 0x100 && guard < 16; guard++) {
            if ((uint8_t)rd(m, cfg(0, dev, fn, at), 1) == 0x11) {
                found = 1;
                break;
            }
            at = (uint8_t)rd(m, cfg(0, dev, fn, at + 1), 1);
        }
        CHECK(found, "walked the capability list to MSI-X (last at %#x)", at);
        CHECK((uint8_t)rd(m, cfg(0, dev, fn, at), 1) == 0x11, "found the MSI-X capability");
        wr(m, cfg(0, dev, fn, at + 2), 0x8000, 2); /* enable, not masked */

        /* Make the GIC able and the CPU ready to take an LPI at 8192. */
        HvGic *g = v.gic;
        g->ctlr |= 2;
        g->cpu[0].igrpen1 = 1;
        g->cpu[0].pmr = 0xf0; /* the priority mask a driver sets before it enables interrupts */
        blk.msix_vector = 1;
        hv_virtio_interrupt(&blk);
        HvPerCpu *p = cpus[0].pc_state;
        CHECK(p->irq_line == 1, "the LPI reached CPU 0 as an IRQ");

        /* Masking the vector stops it: drop the LPI that is still pending first,
           or the line stays up for a reason that is not the new interrupt. */
        g->level[HV_ITS_LPI_BASE] = 0;
        g->latch[HV_ITS_LPI_BASE] = 0;
        g->active[HV_ITS_LPI_BASE] = 0;
        hv_gicv3_update(g);
        wr(m, entry + 12, 1, 4); /* ctrl bit 0: masked */
        hv_virtio_interrupt(&blk);
        CHECK(cpus[0].pc_state->irq_line == 0, "a masked vector raises nothing");
    }
    hv_virt_destroy(&v);
    hv_machine_free(m);
    free(cpus);
    free(m);
    printf("%d checks, %d failures\n", checks, fails);
    return fails != 0;
}
