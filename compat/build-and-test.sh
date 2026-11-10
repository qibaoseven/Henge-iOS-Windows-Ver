#!/bin/sh
# Build the engine and run the unit test that works off Apple platforms.
#
#   sh compat/build-and-test.sh
#
# Apple platforms use the Makefile (`make test`) and need none of this: compat/
# exists only so the same sources compile on Linux/aarch64 (Alpine, iSH,
# Android), where the Mac toolchain is not available.
#
# It changed nothing about how the engine behaves; every file here is a stand-in
# for an Apple header or function. See HANDOVER.md for why it exists.
set -e
cd "$(dirname "$0")/.."
OUT=${OUT:-build/posix}
CC=${CC:-cc}

# -include: core/ calls Darwin's pthread/ulock extras unqualified, so the
#   stand-ins have to land before the sources do.
# -D_GNU_SOURCE: musl hides Dl_info (the profiler names a function by it).
# -latomic: memop.c does a 128-bit compare-and-exchange (CASP).
CFLAGS="-std=c11 -O1 -w -D_GNU_SOURCE -Icompat -Icore -Ihost -Itests -include compat/henge_pthread.h -include compat/henge_ulock.h"

mkdir -p "$OUT"

echo "== compiling the engine"
for f in classify operands translate dispatch memop exception sysreg machine; do
    $CC $CFLAGS -c "core/$f.c" -o "$OUT/$f.o"
done
# The virt machine's pieces, which the Windows direction needs and which build
# without a JIT: their test drives the device callbacks through the MMIO map.
for f in pflash fw_cfg virt dtb its gicv3 pl011 pl031 pl061 pcie timer; do
    $CC $CFLAGS -Idevices -c "devices/$f.c" -o "$OUT/dev_$f.o"
done
# mmu.c must not use SIMD: hv_tlb_refill runs with the guest's vectors live.
$CC $CFLAGS -mgeneral-regs-only -c core/mmu.c -o "$OUT/mmu.o"
$CC $CFLAGS -c host/jit_region.c -o "$OUT/jit_region.o"

echo "== compiling host/enter.S"
# Darwin prefixes its symbols with an underscore; ELF does not. Compile, then
# rename, so the source stays as upstream has it.
$CC -c host/enter.S -o "$OUT/enter.raw.o"
DEFS=""
for s in _hv_chain_thunk _hv_enter _hv_exit _hv_fp_enter _hv_fp_leave _hv_helper_entry \
         _hv_helper_thunk _hv_ijc_thunk _hv_xjump_thunk _hv_helper _hv_tlb_refill _hv_ijc_resolve; do
    DEFS="$DEFS --redefine-sym $s=$(echo "$s" | cut -c2-)"
done
# shellcheck disable=SC2086
objcopy $DEFS "$OUT/enter.raw.o" "$OUT/enter.o"
rm -f "$OUT/enter.raw.o"

OBJS=$(ls "$OUT"/*.o | grep -v '/enter\.o$' | tr '\n' ' ')

echo "== test_mmu"
$CC $CFLAGS tests/test_mmu.c $OBJS "$OUT/enter.o" -latomic -o "$OUT/test_mmu"
# A test keeps a whole HvCpu (2 MB of TLB and more) on its stack.
ulimit -s 65520 2>/dev/null || true
"$OUT/test_mmu"

echo "== test_virt"
$CC $CFLAGS -Idevices tests/test_virt.c $OBJS "$OUT/enter.o" -latomic -o "$OUT/test_virt"
"$OUT/test_virt"

echo "== test_dtb"
$CC $CFLAGS -Idevices tests/test_dtb.c $OBJS "$OUT/enter.o" -latomic -o "$OUT/test_dtb"
"$OUT/test_dtb"

cat <<'EOF'

The other tests assemble tests/*.S, which use Mach-O relocations (@PAGE,
@PAGEOFF) and the Mach-O symbol prefix, or link Apple frameworks. They need a
Mac: `make test`. test_mem additionally needs a JIT, which this host has not.
EOF
