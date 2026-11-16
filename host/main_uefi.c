/* Boot the Windows-ARM64 virtual machine with UEFI and print its serial port.
 *
 * The firmware is the guest: this starts the CPUs at the code flash and runs
 * until the guest halts, printing every byte the PL011 sends. Where the engine
 * has no JIT, translated code cannot run, and this says so instead of printing
 * an empty screen.
 *
 * This used to drive the old `virt` machine (devices/virt.c), which the Windows
 * direction replaced with `winvirt` (devices/winvirt.c). The map, the device
 * list and the way the ACPI set reaches the guest are the new machine's; the
 * command line is unchanged so tools/run-uefi.sh keeps working.
 *
 *   hv_uefi --code edk2-aarch64-code.fd --vars edk2-arm-vars.fd [--ram 512]
 *           [--cpus 4] [--disk windows.img] [--disk-ro] [--save-vars out.fd]
 *           [--verbose]
 */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dispatch.h"
#include "machine.h"
#include "pl011.h"
#include "winvirt.h"

/* What the guest's serial port wrote, plus a line counter. */
static void serial_out(void *opaque, uint8_t c) {
    static FILE *out;
    if (!out) out = stdout;
    fputc(c, out);
    fflush(out);
    (void)opaque;
}

/* How many fw_cfg files the machine published (ACPI set, SMBIOS, table-loader,
   ramfb ...): what the firmware is going to walk. */
static int fw_file_count(const HvFwCfg *fc) { return fc ? fc->nfiles : 0; }

static int fw_has(const HvFwCfg *fc, const char *name) {
    if (!fc) return 0;
    for (int i = 0; i < fc->nfiles; i++)
        if (strncmp(fc->files[i].name, name, sizeof fc->files[0].name - 1) == 0) return 1;
    return 0;
}

int main(int argc, char **argv) {
    const char *code = "firmware/edk2-aarch64-code.fd", *vars = "firmware/edk2-arm-vars.fd";
    const char *save_vars = NULL, *disk_path = NULL;
    const char *cd_paths[3] = {NULL, NULL, NULL};
    int disk_ro = 0;
    int ram_mb = 2048, ncpus = 4, verbose = 0;
    int display_kind = HV_DISPLAY_AUTO;
    uint32_t panel_w = 0, panel_h = 0;
    static struct option opts[] = {{"code", required_argument, 0, 'c'},
                                   {"vars", required_argument, 0, 'v'},
                                   {"ram", required_argument, 0, 'r'},
                                   {"cpus", required_argument, 0, 'n'},
                                   {"disk", required_argument, 0, 'd'},
                                   {"disk-ro", no_argument, 0, 'D'},
                                   {"display", required_argument, 0, 'g'},
                                   {"panel", required_argument, 0, 'p'},
                                   {"cd0", required_argument, 0, '0'},
                                   {"cd1", required_argument, 0, '1'},
                                   {"cd2", required_argument, 0, '2'},
                                   {"save-vars", required_argument, 0, 's'},
                                   {"verbose", no_argument, 0, 'V'},
                                   {0, 0, 0, 0}};
    int o;
    while ((o = getopt_long(argc, argv, "c:v:r:n:s:d:DV0:1:2:g:p:", opts, NULL)) != -1) {
        switch (o) {
        case 'c': code = optarg; break;
        case 'v': vars = optarg; break;
        case 'r': ram_mb = atoi(optarg); break;
        case 'n': ncpus = atoi(optarg); break;
        case 'd': disk_path = optarg; break;
        case 'D': disk_ro = 1; break;
        case 's': save_vars = optarg; break;
        case '0': cd_paths[0] = optarg; break;
        case '1': cd_paths[1] = optarg; break;
        case '2': cd_paths[2] = optarg; break;
        case 'g':
            if (!strcmp(optarg, "ramfb")) display_kind = HV_DISPLAY_RAMFB;
            else if (!strcmp(optarg, "gpu")) display_kind = HV_DISPLAY_VIRTIO_GPU;
            else if (!strcmp(optarg, "auto")) display_kind = HV_DISPLAY_AUTO;
            else {
                fprintf(stderr, "hv_uefi: --display takes ramfb, gpu or auto\n");
                return 2;
            }
            break;
        case 'p':
            if (sscanf(optarg, "%ux%u", &panel_w, &panel_h) != 2) {
                fprintf(stderr, "hv_uefi: --panel takes WIDTHxHEIGHT\n");
                return 2;
            }
            break;
        case 'V': verbose = 1; break;
        default: return 2;
        }
    }
    if (ncpus < 1) ncpus = 1;
    if (ncpus > HV_MAX_CPUS) ncpus = HV_MAX_CPUS;

    HvMachine *m = calloc(1, sizeof *m);
    if (!m) return 1;
    int have_jit = hv_machine_init(m, 256u << 20) == 0;
    if (!have_jit) {
        /* Without JIT memory there is no engine to start: the machine is still
           built below, so that the map, the tree and the firmware's load can be
           checked, but nothing can execute. */
        memset(m, 0, sizeof *m);
    }
    m->log = verbose ? 1 : 0;

    HvCpu *cpus = calloc((size_t)ncpus, sizeof *cpus);
    if (!cpus) return 1;

    HvDisk disk0 = {.fd = -1};
    HvDisk *disk = NULL;
    if (disk_path) {
        if (hv_disk_open(&disk0, disk_path, 0, disk_ro) != 0) {
            fprintf(stderr, "hv_uefi: cannot open the disk image %s\n", disk_path);
            return 3;
        }
        disk = &disk0;
    }

    HvWinVirt b;
    memset(&b, 0, sizeof b);
    if (hv_winvirt_create(&b, m, cpus, ncpus, (uint64_t)ram_mb << 20, NULL, code, vars, disk, NULL, NULL, 0,
                          display_kind, panel_w, panel_h) != 0) {
        fprintf(stderr, "hv_uefi: cannot build the machine (firmware, memory or a window refused)\n");
        if (disk) hv_disk_close(disk);
        return 4;
    }
    for (int i = 0; i < 3; i++) {
        if (!cd_paths[i]) continue;
        if (hv_winvirt_attach_cd(&b, cd_paths[i]) != 0) {
            fprintf(stderr, "hv_uefi: cannot attach the CD image %s\n", cd_paths[i]);
            if (disk) hv_disk_close(disk);
            return 5;
        }
        fprintf(stderr, "hv_uefi: CD tray %d holds %s\n", i + 1, cd_paths[i]);
    }
    b.uart->output = serial_out; /* the firmware's console is here */
    b.uart->output_opaque = NULL;
    hv_winvirt_reset_cpu(&b);

    fprintf(stderr,
            "hv_uefi: pc=%#llx x0=%#llx (device tree) dtb=%llu bytes\n"
            "hv_uefi: code flash %s\n"
            "hv_uefi: vars flash %s\n"
            "hv_uefi: %d CPU(s), %d MiB RAM at %#llx; GIC %#llx, PL011 %#llx, fw_cfg %#llx, GED %#llx\n",
            (unsigned long long)cpus[0].pc, (unsigned long long)cpus[0].x[0],
            (unsigned long long)b.dtb_size, code, vars, ncpus, ram_mb,
            (unsigned long long)WINVIRT_RAM_PA, (unsigned long long)WINVIRT_GIC_DIST_PA,
            (unsigned long long)WINVIRT_UART_PA, (unsigned long long)WINVIRT_FW_CFG_PA,
            (unsigned long long)WINVIRT_GED_PA);
    fprintf(stderr,
            "hv_uefi: ACPI via fw_cfg: %d file(s), rsdp %s, tables %s, loader %s\n"
            "hv_uefi: PCIe ECAM %#llx (%llu MB, %llu buses), MMIO32 %#llx, PIO %#llx\n"
            "hv_uefi: devices: nvme0 %s, nvme1 %s, virtio-net %s, xhci %s, hda %s\n",
            fw_file_count(b.fw_cfg), fw_has(b.fw_cfg, "etc/acpi/rsdp") ? "yes" : "NO",
            fw_has(b.fw_cfg, "etc/acpi/tables") ? "yes" : "NO",
            fw_has(b.fw_cfg, "etc/table-loader") ? "yes" : "NO",
            (unsigned long long)WINVIRT_PCIE_ECAM_PA, (unsigned long long)(WINVIRT_PCIE_ECAM_SZ >> 20),
            (unsigned long long)(WINVIRT_PCIE_ECAM_SZ >> 20),
            (unsigned long long)WINVIRT_PCIE_MMIO_PA, (unsigned long long)WINVIRT_PCIE_PIO_PA,
            b.nvme0 ? (disk_ro ? "read-only" : "read-write") : "absent", b.nvme1 ? "present" : "absent",
            b.net ? "present" : "absent", b.usb ? "present" : "absent", b.hda ? "present" : "absent");

    if (!have_jit) {
        fprintf(stderr,
                "\nhv_uefi: this host has no JIT, so no guest instruction can execute. The machine above\n"
                "hv_uefi: is built and its firmware loaded; run this on a host with JIT execution\n"
                "hv_uefi: (macOS, or a Linux build with the JIT region granted) to see UEFI's output.\n");
        if (disk) hv_disk_close(disk);
        free(cpus);
        hv_machine_free(m);
        free(m);
        return 5;
    }

    /* CPU 0 until the guest halts; every byte the PL011 writes has already gone
       to stdout through serial_out. */
    int r = hv_run(&cpus[0]);
    fprintf(stderr, "\nhv_uefi: CPU 0 left the dispatcher (%d) at pc %#llx\n", r, (unsigned long long)cpus[0].pc);

    /* The variables bank writes through to its file as it goes (CFI01 semantics),
       so there is nothing left to flush; the path is still accepted so that the
       old command lines keep working. */
    if (save_vars && b.flash1 && b.flash1->fd >= 0)
        fprintf(stderr, "hv_uefi: the variables bank is already on disk (%s); --save-vars is a no-op now\n",
                save_vars);

    if (b.flash0) hv_pflash_close(b.flash0);
    if (b.flash1) hv_pflash_close(b.flash1);
    if (disk) hv_disk_close(disk);
    free(cpus);
    hv_machine_free(m);
    free(m);
    return 0;
}
