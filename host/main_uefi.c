/* Boot the virt machine with UEFI and print its serial port.
 *
 * The firmware is the guest: this starts the CPUs at the code flash and runs
 * until the guest halts, printing every byte the PL011 sends. Where the engine
 * has no JIT, translated code cannot run, and this says so instead of printing
 * an empty screen.
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
#include "virt.h"

/* What the guest's serial port wrote, plus a line counter. */
static void serial_out(void *opaque, uint8_t c) {
    static FILE *out;
    if (!out) out = stdout;
    fputc(c, out);
    fflush(out);
    (void)opaque;
}

int main(int argc, char **argv) {
    const char *code = "firmware/edk2-aarch64-code.fd", *vars = "firmware/edk2-arm-vars.fd";
    const char *save_vars = NULL, *disk = NULL;
    int disk_ro = 0;
    int ram_mb = 512, ncpus = 4, verbose = 0;
    static struct option opts[] = {{"code", required_argument, 0, 'c'},
                                   {"vars", required_argument, 0, 'v'},
                                   {"ram", required_argument, 0, 'r'},
                                   {"cpus", required_argument, 0, 'n'},
                                   {"disk", required_argument, 0, 'd'},
                                   {"disk-ro", no_argument, 0, 'D'},
                                   {"save-vars", required_argument, 0, 's'},
                                   {"verbose", no_argument, 0, 'V'},
                                   {0, 0, 0, 0}};
    int o;
    while ((o = getopt_long(argc, argv, "c:v:r:n:s:d:DV", opts, NULL)) != -1) {
        switch (o) {
        case 'c': code = optarg; break;
        case 'v': vars = optarg; break;
        case 'r': ram_mb = atoi(optarg); break;
        case 'n': ncpus = atoi(optarg); break;
        case 'd': disk = optarg; break;
        case 'D': disk_ro = 1; break;
        case 's': save_vars = optarg; break;
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
    HvVirt b;
    memset(&b, 0, sizeof b);
    b.disk_path = disk; /* hv_virt_create keeps these two before it clears the rest */
    b.disk_ro = disk_ro;
    if (hv_virt_create(&b, m, cpus, ncpus, (uint64_t)ram_mb << 20, code, vars) != 0) {
        fprintf(stderr, "hv_uefi: cannot build the machine (firmware, memory or a window refused)\n");
        return 4;
    }
    b.uart->output = serial_out; /* the firmware's console is here */
    b.uart->output_opaque = NULL;
    hv_virt_reset_cpu(&b);

    fprintf(stderr,
            "hv_uefi: pc=%#llx x0=%#llx (device tree) dtb=%llu bytes\n"
            "hv_uefi: code flash %s\n"
            "hv_uefi: vars flash %s\n"
            "hv_uefi: %d CPU(s), %d MiB RAM at %#llx; GIC %#llx, PL011 %#llx, fw_cfg %#llx\n",
            (unsigned long long)cpus[0].pc, (unsigned long long)cpus[0].x[0], (unsigned long long)b.dtb_len, code,
            vars, ncpus, ram_mb, (unsigned long long)VIRT_GUEST_RAM, (unsigned long long)VIRT_GIC_DIST,
            (unsigned long long)VIRT_UART0, (unsigned long long)VIRT_FW_CFG);
    fprintf(stderr,
            "hv_uefi: ACPI %u bytes at %#llx via fw_cfg (RSDP %#llx, FADT %#llx, DSDT %#llx,"
            " MADT %#llx, GTDT %#llx, MCFG %#llx, IORT %#llx, SPCR %#llx)\n"
            "hv_uefi: %d PCIe function(s): virtio-blk %s, virtio-net\n",
            b.acpi_bytes, (unsigned long long)VIRT_ACPI_BASE,
            (unsigned long long)(VIRT_ACPI_BASE + b.acpi.off_rsdp),
            (unsigned long long)(VIRT_ACPI_BASE + b.acpi.off_fadt),
            (unsigned long long)(VIRT_ACPI_BASE + b.acpi.off_dsdt),
            (unsigned long long)(VIRT_ACPI_BASE + b.acpi.off_madt),
            (unsigned long long)(VIRT_ACPI_BASE + b.acpi.off_gtdt),
            (unsigned long long)(VIRT_ACPI_BASE + b.acpi.off_mcfg),
            (unsigned long long)(VIRT_ACPI_BASE + b.acpi.off_iort),
            (unsigned long long)(VIRT_ACPI_BASE + b.acpi.off_spcr), b.blk ? 2 : 1,
            b.blk ? (disk_ro ? "read-only" : "read-write") : "absent");

    if (!have_jit) {
        fprintf(stderr,
                "\nhv_uefi: this host has no JIT, so no guest instruction can execute. The machine above\n"
                "hv_uefi: is built and its firmware loaded; run this on a host with JIT execution\n"
                "hv_uefi: (macOS, or a Linux build with the JIT region granted) to see UEFI's output.\n");
        hv_virt_destroy(&b);
        free(cpus);
        hv_machine_free(m);
        free(m);
        return 5;
    }

    /* CPU 0 until the guest halts; every byte the PL011 writes has already gone
       to stdout through serial_out. */
    int r = hv_run(&cpus[0]);
    fprintf(stderr, "\nhv_uefi: CPU 0 left the dispatcher (%d) at pc %#llx\n", r, (unsigned long long)cpus[0].pc);

    if (save_vars && hv_virt_save_vars(&b, save_vars) != 0)
        fprintf(stderr, "hv_uefi: the variables were not saved\n");
    hv_virt_destroy(&b);
    free(cpus);
    hv_machine_free(m);
    free(m);
    return 0;
}
