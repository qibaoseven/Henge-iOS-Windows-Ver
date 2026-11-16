#!/usr/bin/env bash
# Builds the driver disc the app mounts next to the install media:
#
#   out/drivers.iso   volume label HENGEDRV, plain ISO9660, 8.3 names
#
# The drivers come out of virtio-win (Red Hat's package, BSD-3-Clause); the
# installer and the answer file are in tools/driver-disc/.
#
#   ./tools/make-driver-iso.sh --virtio-win /path/to/virtio-win-1.2.3.iso
#   ./tools/make-driver-iso.sh --virtio-win /path/to/extracted/virtio-win
#
# Everything is staged under build/driver-disc/ so it can be inspected.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGE="$ROOT/build/driver-disc"
OUT="${OUT:-$ROOT/out/drivers.iso}"
SRC="${VIRTIO_WIN:-}"

while [ $# -gt 0 ]; do
    case "$1" in
        --virtio-win) SRC="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ -z "$SRC" ] || [ ! -e "$SRC" ]; then
    cat >&2 <<'EOF'
error: no virtio-win package.

  Download virtio-win (Red Hat's driver ISO) once, then either point at the ISO
  or at a directory it was extracted into:

      ./tools/make-driver-iso.sh --virtio-win ~/Downloads/virtio-win-1.2.3.iso

  The ARM64 drivers for Windows 11 live under viogpudo/w11/ARM64 and
  NetKVM/w11/ARM64, and the signing certificate under cert/.
EOF
    exit 2
fi

rm -rf "$STAGE"
mkdir -p "$STAGE/DRIVERS" "$STAGE/CERT" "$STAGE/ANSWER"

# 1. The disc's own scripts.
cp "$ROOT/tools/driver-disc/install.ps1" "$STAGE/INSTALL.PS1"
cp "$ROOT/tools/driver-disc/install.bat" "$STAGE/INSTALL.BAT"
cp "$ROOT/tools/driver-disc/unattend.xml" "$STAGE/ANSWER/UNATTEND.XML"
cp "$ROOT/tools/driver-disc/README.txt" "$STAGE/README.TXT"
printf 'HENGE driver disc. Do not remove: INSTALL.PS1 looks for this file.\r\n' > "$STAGE/HENGE.TAG"

# 2. The drivers, either out of an ISO or out of an already-extracted tree.
echo "staging drivers from $SRC"
if [ -d "$SRC" ]; then
    for pair in $(grep -v '^#' "$ROOT/tools/driver-disc/manifest.txt" | grep '=' | tr -d ' '); do
        dest="${pair%%=*}"
        src="${pair#*=}"
        # A directory source is laid out like the ISO, so strip the leading slash.
        src_path="$SRC${src}"
        if [ ! -f "$src_path" ]; then
            src_path="$(find "$SRC" -iname "$(basename "$src")" -print -quit 2>/dev/null || true)"
        fi
        if [ ! -f "$src_path" ]; then
            echo "  missing: $src" >&2
            exit 1
        fi
        mkdir -p "$(dirname "$STAGE/$dest")"
        cp "$src_path" "$STAGE/$dest"
        echo "  $src -> $dest"
    done
else
    python3 "$ROOT/tools/iso_pick.py" "$SRC" "$STAGE" "$ROOT/tools/driver-disc/manifest.txt"
fi

# 3. Pack it. Plain ISO9660 (8.3), no Joliet, no boot record: this disc is data
#    for cdfs.sys, not something the firmware boots from.
python3 "$ROOT/tools/make_iso.py" "$STAGE" "$OUT" --volume-id HENGEDRV --verify

echo
echo "driver disc: $OUT ($(du -h "$OUT" | cut -f1))"
echo "contents:"
( cd "$STAGE" && find . -type f | sort | sed 's/^/  /' )
