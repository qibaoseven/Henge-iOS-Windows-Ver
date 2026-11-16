#!/usr/bin/env python3
"""Copy chosen files out of an ISO9660 image (virtio-win ships as one).

  tools/iso_pick.py <iso> <dest-dir> <manifest>

The manifest is lines of `dest/relative/name = /path/inside/iso`, with `#`
comments. Names are listed verbatim as they appear in the disc's directory
records; the Rock Ridge name is what we ask for, because that is what the
project's build scripts read.

pycdlib is used if present (pip install pycdlib); otherwise this falls back to
`7z x` / `bsdtar -xf`, so a build machine needs nothing special.
"""
import os
import shutil
import subprocess
import sys


def read_manifest(path):
    pairs = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line or "=" not in line:
                continue
            dest, src = (s.strip() for s in line.split("=", 1))
            pairs.append((dest, src))
    return pairs


def with_pycdlib(iso_path, dest_root, pairs):
    import pycdlib

    iso = pycdlib.PyCdlib()
    iso.open(iso_path)
    missing = []
    for dest, src in pairs:
        out = os.path.join(dest_root, dest)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        data = None
        # Rock Ridge names are what the project's manifests speak, and pycdlib
        # resolves them directly; ISO9660 8.3 + ";1" names are the fallback.
        for kw in ({"rr_path": src}, {"iso_path": src}):
            try:
                with iso.open_file_from_iso(**kw) as fh:
                    data = fh.read()
                break
            except Exception:
                continue
        if data is None:
            missing.append(src)
            continue
        with open(out, "wb") as f:
            f.write(data)
        print(f"  {src} -> {dest} ({len(data)} bytes)")
    iso.close()
    return missing


def with_external(iso_path, dest_root, pairs):
    tmp = os.path.join(dest_root, ".mount")
    os.makedirs(tmp, exist_ok=True)
    for tool in (["bsdtar", "-xf", iso_path, "-C", tmp], ["7z", "x", f"-o{tmp}", iso_path, "-y"]):
        try:
            subprocess.run(tool, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            break
        except Exception:
            continue
    else:
        raise SystemExit("error: neither pycdlib nor bsdtar/7z could read the ISO")

    missing = []
    for dest, src in pairs:
        src_path = os.path.join(tmp, src.lstrip("/"))
        if not os.path.exists(src_path):
            # fall back to a case-insensitive search
            found = None
            for root, _dirs, files in os.walk(tmp):
                for f in files:
                    if os.path.join(root, f).lower() == src_path.lower():
                        found = os.path.join(root, f)
            src_path = found
        out = os.path.join(dest_root, dest)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        if not src_path or not os.path.exists(src_path):
            missing.append(src)
            continue
        shutil.copy2(src_path, out)
        print(f"  {src} -> {dest} ({os.path.getsize(out)} bytes)")
    shutil.rmtree(tmp, ignore_errors=True)
    return missing


def main(argv):
    if len(argv) < 4:
        print(__doc__)
        return 2
    iso_path, dest_root, manifest = argv[1], argv[2], argv[3]
    pairs = read_manifest(manifest)
    os.makedirs(dest_root, exist_ok=True)
    try:
        import pycdlib  # noqa: F401
        missing = with_pycdlib(iso_path, dest_root, pairs)
    except ImportError:
        missing = with_external(iso_path, dest_root, pairs)
    for m in missing:
        print(f"  missing: {m}", file=sys.stderr)
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
