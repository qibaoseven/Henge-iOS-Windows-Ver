#ifndef HENGEVM_ITS_H
#define HENGEVM_ITS_H

#include <pthread.h>
#include <stdint.h>

#include "gicv3.h"
#include "machine.h"

/* The GICv3 Interrupt Translation Service: what turns a device's MSI write into
   an LPI on a CPU.

   Without it GICv3 has no MSI at all -- LPIs are delivered through the ITS and
   nowhere else -- so a PCIe device can only interrupt on its legacy INTx line.
   UEFI does not care (its drivers poll); Windows' virtio drivers do ask for MSI
   and give up without it, which is why this exists.

   The guest's driver keeps a command queue in RAM and writes commands into it:
   MAPD (device to collection), MAPC (collection to a redistributor), MAPTI (a
   device's event to a physical LPI). A device's MSI then arrives as a write of
   the device's EventID to GITS_TRANSLATER, and the ITS looks it up and pends
   the LPI on the collection's CPU.

   The tables' layout is the implementation's choice; GITS_TYPER states it. */

#define HV_ITS_LPI_BASE HV_GIC_LPI_BASE
#define HV_ITS_LPI_COUNT HV_GIC_LPI_COUNT
#define HV_ITS_MAX_EVENTS 8

typedef struct HvItsDev {
    uint32_t device;
    int collection;
    int valid;
    struct {
        uint32_t event;
        int lpi; /* offset above the base */
    } ev[HV_ITS_MAX_EVENTS];
    int nev;
} HvItsDev;

typedef struct HvIts {
    HvMachine *m;
    HvGic *gic;
    pthread_mutex_t mu;
    uint32_t ctlr;     /* GITS_CTLR, [0] Enable */
    uint64_t typer;    /* GITS_TYPER, read-only */
    uint64_t cbaser;   /* GITS_CBASER */
    uint32_t creadr;   /* GITS_CREADR: the next command to run */
    uint32_t cwriter;  /* GITS_CWRITER: where the guest stopped writing */
    int (*rd_cpu)(uint32_t rd_base); /* a redistributor's base -> its CPU index */
    HvItsDev *dev;
    int ndev, capdev;
    int coll[HV_MAX_CPUS]; /* a collection's CPU, -1 until MAPC */
    int ncoll;
} HvIts;

/* Maps the ITS window (QEMU virt: 0x08080000, 0x20000) and attaches it to the
   GIC whose redistributors it delivers to. rd_cpu maps a redistributor's base
   address to a CPU index. NULL if the window is taken. */
HvIts *hv_its_create(HvMachine *m, HvGic *gic, uint64_t pa, uint64_t size, int (*rd_cpu)(uint32_t rd_base));
/* Runs the commands the guest queued since last time. */
void hv_its_poll(HvIts *its);
/* An MSI: the device's EventID, as a write to GITS_TRANSLATER delivers it. */
void hv_its_signal(HvIts *its, uint32_t device, uint32_t event);
/* The INTID an LPI offset stands for. */
static inline uint32_t hv_its_intid(int offset) { return HV_ITS_LPI_BASE + (uint32_t)offset; }

#endif
