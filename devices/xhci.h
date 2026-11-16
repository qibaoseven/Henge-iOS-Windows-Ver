#ifndef HENGEVM_XHCI_H
#define HENGEVM_XHCI_H

#include <pthread.h>

#include "pcie.h"

#include "usb_storage.h"

/* xHCI 1.0 USB host controller (MSI-X + INTx) with:
   - Port 1: USB HID Boot Keyboard
   - Port 2: USB HID Absolute Tablet (0..32767 X/Y, L/R/M buttons, scroll wheel)
   - Port 3: USB Mass Storage CD-ROM (Bulk-Only Transport), attached on demand.
     It reports INQUIRY peripheral device type 5, which is the only way Windows
     binds cdfs.sys and therefore the only way an install image larger than
     4 GiB can be read.
   Zero guest drivers required in EDK2 UEFI or Windows 11 ARM64. */
#define HV_XHCI_SLOTS 8
#define HV_XHCI_PORTS 4 /* 1 keyboard, 2 tablet, 3 and 4 CD-ROM trays */
#define HV_XHCI_INTRS 4
#define HV_XHCI_EPS 32 /* device context index 1..31 */
#define HV_XHCI_REPORTS 256

/* Bulk endpoint data channel indices for the CD (EP1 OUT = 2, EP1 IN = 3). */
#define HV_CD_DCI_OUT 2
#define HV_CD_DCI_IN 3

typedef struct HvXhciRing {
    uint64_t dequeue; /* guest PA of the next TRB */
    int ccs;          /* consumer cycle state */
} HvXhciRing;

typedef struct HvXhciSlot {
    int used;
    int port;         /* 1 = keyboard, 2 = tablet */
    int configured;
    uint8_t protocol; /* 0 = boot, 1 = report */
    uint8_t idle;
    HvXhciRing ep[HV_XHCI_EPS];
    int ep_running[HV_XHCI_EPS];
} HvXhciSlot;

typedef struct HvXhci {
    HvPciDev pci;
    HvMachine *m;
    /* operational */
    uint32_t usbcmd, usbsts, dnctrl, config;
    uint64_t dcbaap;
    HvXhciRing cmd;
    int cmd_running;
    uint32_t portsc[HV_XHCI_PORTS];
    /* interrupters and their event rings */
    struct HvXhciIntr {
        uint32_t iman, imod, erstsz;
        uint64_t erstba, erdp;
        uint64_t seg_base;
        uint32_t seg_size, seg_index, index;
        int pcs;
    } intr[HV_XHCI_INTRS];

    HvXhciSlot slots[HV_XHCI_SLOTS + 1];
    int kbd_slot;
    int tab_slot;
    /* Two CD trays (ports 3 and 4): setup media and payload media at once. */
    int cd_slot[2];
    int cd_attached[2];
    HvUsbCd cd[2];
    uint8_t cd_buf[65536]; /* staging for bulk-IN chunks; CPU thread only */

    pthread_mutex_t mu;
    /* Port 1 keyboard reports (8 bytes each) */
    uint8_t reports[HV_XHCI_REPORTS][8];
    uint32_t rhead, rtail;
    uint8_t state[8];
    /* Port 2 absolute tablet reports (6 bytes: buttons, x_lo, x_hi, y_lo, y_hi, wheel) */
    uint8_t tab_reports[HV_XHCI_REPORTS][6];
    uint32_t thead, ttail;
    uint8_t tab_state[6];

    uint64_t delivered;
} HvXhci;

HvXhci *hv_xhci_create(HvMachine *m, HvPcie *bus);
/* Any thread: HID usage page 7 key down/up (e.g. 4 = A; 0xe0-0xe7 modifiers). */
void hv_xhci_key(HvXhci *x, uint8_t usage, int down);
/* Any thread: Linux input key code down/up. */
void hv_xhci_key_linux(HvXhci *x, uint16_t code, int down);
/* Any thread: USB HID Absolute Tablet event (x, y in 0..32767; buttons bit0=L, bit1=R, bit2=M; wheel -127..127). */
void hv_xhci_tablet(HvXhci *x, int abs_x, int abs_y, uint8_t buttons, int8_t wheel);
/* CPU thread: hand waiting keyboard & tablet reports to the guest, and move any
   outstanding CD bulk transfers. */
void hv_xhci_poll(HvXhci *x);

/* Any thread before the guest boots: put an ISO on the first free CD tray (port
   3, then 4) as a read-only CD-ROM. Returns 0 on success, or -1 if the image
   cannot be opened or both trays are full. */
int hv_xhci_attach_cd(HvXhci *x, const char *iso_path);
/* Close both trays (between two boots; the ports stay present but empty). */
void hv_xhci_detach_cd(HvXhci *x);

#endif
