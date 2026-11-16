/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HV_WINVIRT_H
#define HV_WINVIRT_H

#include <stddef.h>
#include <stdint.h>

#include "acpi.h"
#include "disk.h"
#include "fb_display.h"
#include "fdt.h"
#include "fw_cfg.h"
#include "gicv3.h"
#include "machine.h"
#include "nvme.h"
#include "pcie.h"
#include "pflash.h"
#include "pl011.h"
#include "pl031.h"
#include "pl061.h"
#include "hda.h"
#include "virtio_net.h"
#include "virtio_snd.h"
#include "virtio_gpu.h"
#include "xhci.h"

/* Standard ARM64 'virt' physical memory map for EDK2 UEFI + Windows 11 ARM64 */
#define WINVIRT_FLASH0_PA     0x00000000ULL
#define WINVIRT_FLASH1_PA     0x04000000ULL
#define WINVIRT_GIC_DIST_PA    0x08000000ULL
#define WINVIRT_GIC_ITS_PA     0x08080000ULL
#define WINVIRT_GIC_TRANSLATER 0x08090040ULL
#define WINVIRT_GIC_REDIST_PA  0x080a0000ULL
#define WINVIRT_UART_PA       0x09000000ULL
#define WINVIRT_RTC_PA        0x09010000ULL
#define WINVIRT_FW_CFG_PA     0x09020000ULL
#define WINVIRT_GPIO_PA       0x09030000ULL
#define WINVIRT_GED_PA        0x09080000ULL
#define WINVIRT_PCIE_MMIO_PA  0x10000000ULL
#define WINVIRT_PCIE_MMIO_SZ  0x2eff0000ULL
#define WINVIRT_PCIE_PIO_PA   0x3eff0000ULL
#define WINVIRT_PCIE_PIO_SZ   0x00010000ULL
#define WINVIRT_RAM_PA        0x40000000ULL
#define WINVIRT_PCIE_ECAM_PA  0x4010000000ULL
#define WINVIRT_PCIE_ECAM_SZ  0x10000000ULL
#define WINVIRT_PCIE_HIGH_PA  0x8000000000ULL
#define WINVIRT_PCIE_HIGH_SZ  0x8000000000ULL

#define WINVIRT_INTID_UART    (32 + 1)
#define WINVIRT_INTID_RTC     (32 + 2)
#define WINVIRT_INTID_PCIE    (32 + 3) /* SPIs 3..6 -> INTIDs 35..38 */
#define WINVIRT_INTID_GPIO    (32 + 7)
#define WINVIRT_INTID_GED     (32 + 9) /* GSI 41 */
#define WINVIRT_MSI_BASE      80       /* INTIDs 80..143 (64 MSI-X vectors) */
#define WINVIRT_MSI_COUNT     64

/* Which display hardware the guest sees. Only two of them are any use to an
   ARM64 Windows guest: ramfb has no Windows driver at all, and virtio-gpu is the
   one viogpudo binds. The others QEMU offers (virtio-vga, bochs, qxl, stdvga)
   need VGA registers or x64-only drivers, so they are not modelled here. */
typedef enum {
    HV_DISPLAY_RAMFB = 0,      /* firmware/Linux only */
    HV_DISPLAY_VIRTIO_GPU = 1, /* 1af4:1050, 2D */
    HV_DISPLAY_AUTO = 2,       /* both: firmware gets ramfb, Windows picks the GPU */
} HvDisplayKind;

typedef struct HvWinVirt {
    HvMachine *m;
    HvCpu *cpu;
    int ncpus;

    uint8_t *ram;
    uint64_t ram_size;
    uint8_t *dtb;
    size_t dtb_size;

    HvPflash *flash0;
    HvPflash *flash1;
    HvGic *gic;
    HvGicIts *its;
    HvPl011 *uart;
    HvPl031 *rtc;
    HvPl061 *gpio;
    HvFwCfg *fw_cfg;
    HvFbDisplay *fb;
    HvPcie *pcie;
    HvVirtioNet *net;
    HvNvme *nvme0;
    HvNvme *nvme1;
    HvXhci *usb;
    HvVirtioGpu *gpu;
    int display_kind;
    HvHda *hda;
    HvVirtioSnd *snd;

    uint32_t ged_events;
    void (*poll)(void);
} HvWinVirt;

uint8_t *hv_alloc_no_footprint(uint64_t size);
int hv_winvirt_create(HvWinVirt *b, HvMachine *m, HvCpu *cpu, int ncpus, uint64_t ram_size,
                      void *prealloc_ram, const char *code_fd_path, const char *vars_fd_path,
                      HvDisk *disk0, HvDisk *disk1, const uint8_t mac[6], int enable_sound,
                      int display_kind, uint32_t panel_w, uint32_t panel_h);
/* Put an ISO on xHCI port 3 as a read-only CD-ROM (before the CPU starts).
   Call twice to attach two CDs (setup media + payload media). */
int hv_winvirt_attach_cd(HvWinVirt *b, const char *iso_path);
void hv_winvirt_detach_cds(HvWinVirt *b);
void hv_winvirt_reset_cpu(HvWinVirt *b);
void hv_winvirt_power_button(HvWinVirt *b);

#endif
