#ifndef HENGEVM_PCIE_H
#define HENGEVM_PCIE_H

#include <stdint.h>

#include "gicv3.h"

/* A PCIe host bridge as QEMU's gpex and virt's pci-host-ecam-generic build one:
   ECAM configuration space over every bus the window covers, one MMIO window,
   and (now) MSI through the ITS.

   Windows walks the whole bus range, reads each function's extended
   capabilities, and reassigns every BAR before it will use a device, so all
   three have to work: a bridge that only answers bus 0 makes the OS stop at
   the first empty bus.

   The bus count is what the ECAM window can address, not a choice: 32 devs x 8
   functions x 4 KB is 1 MB per bus, and QEMU's window is 16 MB, so 16 buses.
   Claiming 256 would have the OS read configuration space that is not there. */
#define HV_PCI_MAX_BUS 16
#define HV_PCI_SLOTS 32
#define HV_PCI_FUNCS 8
#define HV_PCI_MAX_DEV (HV_PCI_SLOTS * HV_PCI_FUNCS)

struct HvPcie;

typedef struct HvPciDev {
    struct HvPcie *bus;
    int devfn;                /* bus << 8 | device << 3 | function */
    uint8_t config[4096];     /* the standard header and extended capabilities */
    uint8_t wmask[256];       /* writable bits of the standard header */
    uint64_t bar_size[6];     /* 0: no BAR */
    uint8_t bar64[6];
    int intx;                 /* current INTA level (legacy; unused once MSI is on) */
    void *opaque;
    uint64_t (*bar_read)(struct HvPciDev *d, int bar, uint64_t off, unsigned size);
    void (*bar_write)(struct HvPciDev *d, int bar, uint64_t off, uint64_t v, unsigned size);
    /* MSI: called when the guest writes an MSI or MSI-X message. A device that
       has none leaves these NULL. */
    void (*msi)(void *opaque, int vector);
    int msix_vectors;
    int msix_current_vector;  /* which vector the next interrupt uses (per-queue, set by the device) */
    uint8_t *msix_table;      /* in guest memory; the BAR window maps it */
    uint64_t msix_table_pa;
    uint64_t msix_pba_pa;
    uint8_t cap_next;         /* where the next capability goes (see cap_add) */
} HvPciDev;

typedef struct HvPcie {
    HvMachine *m;
    HvGic *gic;
    void *its;                /* HvIts*, or NULL: no MSI */
    int intx_base;
    int line_count[HV_PCI_MAX_DEV];
    HvPciDev *dev[HV_PCI_MAX_DEV]; /* indexed by bus << 8 | dev << 3 | fn, but only bus 0..255 and 32 devs are used */
    HvPciDev bridge;
    uint64_t ecam_pa, ecam_size;
    uint64_t mmio_base, mmio_size;
    uint64_t pio_base, pio_size;
} HvPcie;

HvPcie *hv_pcie_create(HvMachine *m, HvGic *gic, uint64_t ecam_pa, uint64_t ecam_size, uint64_t mmio_pa,
                       uint64_t mmio_size, int intx_intid);
/* Attach the ITS so devices that expose MSI-X have somewhere to send it. */
void hv_pcie_set_its(HvPcie *p, void *its);
/* Fills in the header; the caller then adds BARs and capabilities. */
void hv_pci_init(HvPciDev *d, uint16_t vendor, uint16_t device, uint32_t class_rev, uint16_t sub_vendor,
                 uint16_t sub_id);
void hv_pci_add_bar(HvPciDev *d, int bar, uint64_t size, int is64);
/* Adds an MSI-X capability (ID 0x11) with `n` vectors. The table and PBA live in
   the device's BAR at `table_off` and `pba_off`, as QEMU lays them out. */
int hv_pci_add_msix(HvPciDev *d, int n, int bar, uint32_t table_off, uint32_t pba_off);
/* Adds the PCIe capability (ID 0x10): device type and no link training needed. */
int hv_pci_add_pcie_cap(HvPciDev *d);
int hv_pcie_attach(HvPcie *p, HvPciDev *d); /* returns devfn, or -1 */
uint64_t hv_pci_bar_addr(HvPciDev *d, int bar);
void hv_pci_set_intx(HvPciDev *d, int level);
/* What a device calls to raise an interrupt now: an MSI through the ITS if it
   has MSI-X enabled, the legacy line otherwise. */
void hv_pci_interrupt(HvPciDev *d);

#endif
