#!/bin/sh
# Boot the Windows-ARM64 machine (devices/winvirt.c) with an arm64 UEFI
# firmware, and print what its serial port says.
#
#   sh tools/run-uefi.sh [code.fd] [vars.fd] [ram_mb]
#
# Nothing here fakes a result. Without a JIT the engine cannot execute guest
# code, so this reports the machine it built and says so, rather than looking
# like a boot that produced no output.
set -e
cd "$(dirname "$0")/.."

CODE=${1:-${EDK2_CODE:-firmware/edk2-aarch64-code.fd}}
VARS=${2:-${EDK2_VARS:-firmware/edk2-arm-vars.fd}}
RAM=${3:-${HV_RAM_MB:-2048}}

for f in "$CODE" "$VARS"; do
    if [ ! -f "$f" ]; then
        cat >&2 <<EOF
run-uefi: no firmware at $f

ArmVirtQemu's two flash images are needed: the code bank (read-only) and the
variables bank (writable), 64 MB each. Get them from a QEMU installation
(they ship as edk2-aarch64-code.fd / edk2-arm-vars.fd) or build them from
edk2's ArmVirtPkg. Then either put them in firmware/ or pass the paths:

    sh tools/run-uefi.sh /path/to/edk2-aarch64-code.fd /path/to/edk2-arm-vars.fd
EOF
        exit 2
    fi
done

CC=${CC:-cc}
BIN=build/posix/hv_uefi
mkdir -p build/posix
CFLAGS="-std=c11 -O1 -w -D_GNU_SOURCE -Icompat/shadow -Icompat -Icore -Ihost -Idevices -include compat/henge_pthread.h -include compat/henge_ulock.h"
OBJS=""
needs_build=0
[ -x "$BIN" ] || needs_build=1
for f in core/classify core/operands core/translate core/dispatch core/memop core/exception core/sysreg core/machine \
         devices/winvirt devices/fdt devices/fw_cfg devices/pflash devices/gicv3 devices/pl011 devices/pl031 \
         devices/pl061 devices/pcie devices/virtio devices/virtio_net devices/virtio_snd devices/virtio_blk devices/virtio_gpu \
         devices/acpi devices/timer devices/nvme devices/hda devices/xhci devices/fb_display devices/usb_storage \
         host/disk host/jit_region host/main_uefi; do
    o=build/posix/vb_$(basename "$f").o
    OBJS="$OBJS $o"
    if [ ! -f "$o" ] || [ "$f.c" -nt "$o" ] || [ -n "$(find core devices host -name '*.h' -newer "$o" 2>/dev/null | head -1)" ]; then
        $CC $CFLAGS -c "$f.c" -o "$o"
    fi
done
$CC $CFLAGS -mgeneral-regs-only -c core/mmu.c -o build/posix/vb_mmu.o
if [ ! -f build/posix/vb_enter.o ]; then
    $CC -c host/enter.S -o build/posix/vb_enter.raw.o
    DEFS=""
    for s in _hv_chain_thunk _hv_enter _hv_exit _hv_fp_enter _hv_fp_leave _hv_helper_entry \
             _hv_helper_thunk _hv_ijc_thunk _hv_xjump_thunk _hv_helper _hv_tlb_refill _hv_ijc_resolve; do
        DEFS="$DEFS --redefine-sym $s=$(echo "$s" | cut -c2-)"
    done
    # shellcheck disable=SC2086
    objcopy $DEFS build/posix/vb_enter.raw.o build/posix/vb_enter.o
    rm -f build/posix/vb_enter.raw.o
fi
$CC $CFLAGS $OBJS build/posix/vb_mmu.o build/posix/vb_enter.o -latomic -lpthread -o "$BIN"
( $needs_build ) >/dev/null 2>&1 || true

EXTRA=""
[ -n "${HV_CD0:-}" ] && EXTRA="$EXTRA --cd0 $HV_CD0"
[ -n "${HV_CD1:-}" ] && EXTRA="$EXTRA --cd1 $HV_CD1"
[ -n "${HV_CD2:-}" ] && EXTRA="$EXTRA --cd2 $HV_CD2"
# shellcheck disable=SC2086
exec "$BIN" --code "$CODE" --vars "$VARS" --ram "$RAM" $EXTRA
