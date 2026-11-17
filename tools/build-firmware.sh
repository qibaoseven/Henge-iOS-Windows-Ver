#!/usr/bin/env bash
# Builds the two pflash images the board expects: code.fd (64 MB, read-only UEFI)
# and vars.fd (64 MB, writable NVRAM). Both are ordinary AArch64 ArmVirtQemu
# builds -- the same firmware QEMU's `virt` machine runs -- because the board in
# devices/winvirt.c is that machine with a Windows-shaped set of devices.
#
# Two ways in:
#   ./tools/build-firmware.sh --package      take the distro's AAVMF build (seconds)
#   ./tools/build-firmware.sh --source       build edk2 from git (minutes, reproducible)
#
# Output lands in out/firmware/{code.fd,vars.fd}; copy them into the app bundle
# as code.fd / vars.fd (WinStore copies them into each machine's folder).
#
# Why 64 MB: devices/pflash.h maps one bank per 64 MB, exactly like QEMU, and
# EDK2's variable driver reads PcdFlashNvStorage* offsets relative to the bank.
# Anything else and the firmware asserts before it prints.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/out/firmware"
BANK=$((64 * 1024 * 1024))
MODE="${1:---package}"

say() { printf '\033[1m%s\033[0m\n' "$*"; }

mkdir -p "$OUT"

pad_to_bank() { # $1 src  $2 dst
    local src="$1" dst="$2" size
    size=$(stat -c %s "$src" 2>/dev/null || stat -f %z "$src")
    if [ "$size" -gt "$BANK" ]; then
        echo "error: $src is $size bytes, larger than one 64 MB bank" >&2
        return 1
    fi
    cp "$src" "$dst"
    # Sparse padding: the tail of a flash bank is erased (0xFF), but EDK2 only
    # reads the FV it can find, so zeros are equally fine and cost nothing on disk.
    truncate -s "$BANK" "$dst"
    echo "  $(basename "$dst"): $size bytes -> padded to $((BANK / 1024 / 1024)) MB"
}

sanity() {
    local code="$1" vars="$2"
    python3 - "$code" "$vars" <<'PY'
import sys
code, vars_ = sys.argv[1], sys.argv[2]
with open(code, 'rb') as f:
    head = f.read(0x1000)
if b'_FVH' not in head[:0x100]:
    print('  warning: no FV header near the start of code.fd -- is this really a UEFI build?')
else:
    print('  code.fd: firmware volume header found')
with open(vars_, 'rb') as f:
    vh = f.read(0x2000)
if all(b == 0xff for b in vh):
    print('  vars.fd: erased NVRAM (the variable driver will format it)')
elif all(b == 0 for b in vh):
    print('  warning: vars.fd is all zeros; EDK2 cannot format that. Use the package/source template.')
else:
    print('  vars.fd: existing variable store template')
PY
}

# edk2 pins twelve submodules, three of which live under UnitTestFrameworkPkg
# (cmocka, googletest, subhook). Only edk2's host unit tests use those; ArmVirtQemu
# never does -- but `git submodule update --init --recursive` gives up on the whole
# tree when one of them fails, which is what CI saw:
#
#   fatal: could not read Username for 'https://github.com': Device not configured
#   fatal: clone of 'https://github.com/Zeex/subhook.git' ... a second time, aborting
#
# Zeex/subhook no longer resolves, and GitHub answers an anonymous clone of a
# repository it cannot show with a 401, so git asks for a username and has no
# terminal to ask on. Fetch each submodule on its own instead: a UnitTestFramework
# one that fails is reported and skipped, anything else is still fatal.
init_edk2_submodules() {
    _repo="$1"
    _optional_failed=""
    while read -r _path; do
        case "$_path" in
            UnitTestFrameworkPkg/*) _optional=1 ;;
            *) _optional=0 ;;
        esac
        printf '  submodule %s\n' "$_path"
        if GIT_TERMINAL_PROMPT=0 git -C "$_repo" submodule update --init -- "$_path" 2>&1 | sed 's/^/    /'; then
            continue
        fi
        if [ "$_optional" = 1 ]; then
            echo "    skipped: only edk2's host unit tests use this one, not ArmVirtQemu"
            _optional_failed="$_optional_failed $_path"
        else
            echo "error: could not fetch $_path, and ArmVirtQemu needs it" >&2
            return 1
        fi
    done <<EOF
$(git config -f "$_repo/.gitmodules" --get-regexp '^submodule\..*\.path$' | cut -d' ' -f2-)
EOF
    if [ -n "$_optional_failed" ]; then
        say "Not fetched (optional):$_optional_failed"
    fi
    return 0
}

if [ "$MODE" = "--package" ]; then
    say "Looking for a distro AArch64 EDK2 build"
    candidate_dirs=(
        /usr/share/AAVMF
        /usr/share/qemu-efi-aarch64
        /usr/share/edk2/aarch64
        /opt/homebrew/share/qemu
        "${HOME}/Homebrew/share/qemu"
    )
    code_src="" vars_src=""
    for d in "${candidate_dirs[@]}"; do
        [ -d "$d" ] || continue
        for c in "$d/AAVMF_CODE.fd" "$d/QEMU_EFI.fd" "$d/edk2-aarch64-code.fd"; do
            [ -f "$c" ] && code_src="$c" && break
        done
        for v in "$d/AAVMF_VARS.fd" "$d/QEMU_VARS.fd" "$d/edk2-arm-vars.fd"; do
            [ -f "$v" ] && vars_src="$v" && break
        done
        [ -n "$code_src" ] && [ -n "$vars_src" ] && break
    done

    if [ -z "$code_src" ] || [ -z "$vars_src" ]; then
        cat >&2 <<'EOF'
error: no AArch64 EDK2 build found on this machine.

  Debian/Ubuntu:  sudo apt install qemu-efi-aarch64     (AAVMF_CODE.fd / AAVMF_VARS.fd)
  Fedora:         sudo dnf install edk2-aarch64         (QEMU_EFI.fd / QEMU_VARS.fd)
  macOS:          brew install qemu                      (share/qemu/edk2-aarch64-code.fd)

Or build it:  ./tools/build-firmware.sh --source
EOF
        exit 1
    fi

    say "Using $code_src"
    pad_to_bank "$code_src" "$OUT/code.fd"
    pad_to_bank "$vars_src" "$OUT/vars.fd"
    sanity "$OUT/code.fd" "$OUT/vars.fd"
else
    say "Building edk2 ArmVirtQemu (AARCH64, RELEASE)"
    EDK2="${EDK2:-$ROOT/third_party/edk2}"
    if [ ! -d "$EDK2" ]; then
        say "Cloning edk2 into $EDK2 (this pulls ~200 MB of submodules)"
        # Pin a known-good tag; submodules are pinned by it, so a shallow
        # submodule clone would fail on the exact sha -- take them at depth but
        # let git fetch the pinned commit.
        GIT_TERMINAL_PROMPT=0 git clone --depth 1 --branch edk2-stable202408 \
            https://github.com/tianocore/edk2.git "$EDK2"
        init_edk2_submodules "$EDK2"
    fi

    JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

    # Toolchain: a native AArch64 gcc, or an x86 cross compiler, or clang for XCODE5.
    TOOLCHAIN=""
    if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
        TOOLCHAIN="GCC5"; PREFIX="aarch64-linux-gnu-"
    elif [ "$(uname -s)" = "Darwin" ] && [ "$(uname -m)" = "arm64" ]; then
        TOOLCHAIN="XCODE5"; PREFIX=""
    elif command -v clang >/dev/null 2>&1; then
        TOOLCHAIN="CLANGDWARF"; PREFIX=""
    else
        echo "error: no AArch64 toolchain found (need aarch64-linux-gnu-gcc or clang)" >&2
        exit 1
    fi
    say "Toolchain: $TOOLCHAIN"

    # edk2's `build` is a wrapper script that only exists on PATH after its
    # wrappers directory is added; calling it without edksetup is the most common
    # way this step fails silently.
    ( cd "$EDK2"
      export WORKSPACE="$EDK2"
      export EDK_TOOLS_PATH="$EDK2/BaseTools"
      export PACKAGES_PATH="$EDK2"
      [ -f pip-requirements.txt ] && python3 -m pip install -q -r pip-requirements.txt || true
      make -C BaseTools -j"$JOBS"
      export PATH="$EDK2/BaseTools/BinWrappers/PosixLike:$PATH"
      build -a AARCH64 -t "$TOOLCHAIN" -b RELEASE -p ArmVirtPkg/ArmVirtQemu.dsc \
            -D "GCC5_AARCH64_PREFIX=$PREFIX" -n "$JOBS"
    )
    Build="$EDK2/Build/ArmVirtQemu-AARCH64/RELEASE_${TOOLCHAIN}"
    if [ ! -f "$Build/FV/QEMU_EFI.fd" ]; then
        echo "error: build finished but $Build/FV/QEMU_EFI.fd is not there" >&2
        echo "       (toolchain was $TOOLCHAIN; check the build log above)" >&2
        exit 1
    fi
    pad_to_bank "$Build/FV/QEMU_EFI.fd" "$OUT/code.fd"
    if [ -f "$Build/FV/QEMU_VARS.fd" ]; then
        pad_to_bank "$Build/FV/QEMU_VARS.fd" "$OUT/vars.fd"
    else
        # ArmVirtQemu builds its variable store into the code FV when the DSC says
        # so; without a separate template EDK2 can still format a blank bank.
        echo "  no QEMU_VARS.fd in the build; writing an erased (0xFF) 64 MB bank"
        python3 - "$OUT/vars.fd" <<'PYV'
import sys
with open(sys.argv[1], 'wb') as f:
    f.write(b'\xff' * (64 << 20))
PYV
    fi
    sanity "$OUT/code.fd" "$OUT/vars.fd"
fi

say "Done: $OUT/code.fd, $OUT/vars.fd"
cat <<'EOF'

Next:
  * put them in the app bundle (Resources/code.fd, Resources/vars.fd), or
  * for a quick host test: HV_FIRMWARE=out/firmware ./tools/run-winvm.sh
EOF