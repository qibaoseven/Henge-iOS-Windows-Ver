#!/usr/bin/env bash
# Puts the three things the app bundle must contain where XcodeGen picks them up:
#
#   ios/Henge/Resources/code.fd    UEFI (64 MB pflash bank 0, read-only)
#   ios/Henge/Resources/vars.fd    NVRAM (64 MB pflash bank 1, writable)
#   ios/Henge/Resources/drivers.iso  the driver disc
#
# WinStore.prepare() looks these up as Bundle.main resources with exactly those
# names, so a typo here is a machine that refuses to boot with "no firmware".
#
#   ./tools/stage-app-resources.sh          copy what exists, report what does not
#   ./tools/stage-app-resources.sh --check  only report (non-zero if something is missing)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_FW="$ROOT/out/firmware"
SRC_ISO="$ROOT/out/drivers.iso"
DST="$ROOT/ios/Henge/Resources"
CHECK=0
[ "${1:-}" = "--check" ] && CHECK=1

say() { printf '\033[1m%s\033[0m\n' "$*"; }
missing=0

size_of() { stat -c %s "$1" 2>/dev/null || stat -f %z "$1"; }

stage() { # $1 src  $2 dst-name  $3 描述
    local src="$1" name="$2" what="$3"
    if [ ! -f "$src" ]; then
        printf '  %-12s 缺：%s\n' "$name" "$what"
        missing=$((missing + 1))
        return 0
    fi
    if [ "$CHECK" = 1 ]; then
        printf '  %-12s 有：%s（%s 字节）\n' "$name" "$src" "$(size_of "$src")"
    else
        mkdir -p "$DST"
        cp "$src" "$DST/$name"
        chmod u+w "$DST/$name"
        printf '  %-12s <- %s（%s 字节）\n' "$name" "$src" "$(size_of "$DST/$name")"
    fi
}

say "App 资源：$DST"
stage "$SRC_FW/code.fd" code.fd "先跑 tools/build-firmware.sh（或 CI 的 firmware 工作流）"
stage "$SRC_FW/vars.fd" vars.fd "同上"
stage "$SRC_ISO" drivers.iso "先跑 tools/make-driver-iso.sh"

# The firmware banks are 64 MB each by design (devices/pflash.h); a short file
# would boot to nothing, so check the size and the FV header rather than find out
# on the device.
if [ -f "$DST/code.fd" ] || [ "$CHECK" = 1 ] && [ -f "$SRC_FW/code.fd" ]; then
    code="${DST}/code.fd"
    [ "$CHECK" = 1 ] && code="$SRC_FW/code.fd"
    sz=$(size_of "$code")
    if [ "$sz" -lt $((60 * 1024 * 1024)) ]; then
        echo "  warning: code.fd 只有 $sz 字节，pflash 期望 64 MB"
    fi
    if ! head -c 4096 "$code" | grep -q '_FVH'; then
        echo "  warning: code.fd 开头没有 FV 头（_FVH），可能不是 UEFI 固件"
    fi
fi

say ""
if [ "$missing" -gt 0 ]; then
    if [ "$CHECK" = 1 ]; then
        echo "$missing 个资源还没就位。"
        exit 1
    fi
    echo "已复制现有的，还差 $missing 个；补齐后再跑一次。"
else
    echo "三个资源齐了。接下来：cd ios && xcodegen && xcodebuild"
fi
