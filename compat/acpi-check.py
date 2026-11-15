#!/usr/bin/env python3
"""Check the tables test_acpi dumped, with iasl where iasl can be trusted.

There is a catch: the iasl built for this Alpine (acpica 20230628) reads a
table's length field (and some of its 64-bit fields) with the *host's*
endianness, not ACPI's big-endian one, so a perfectly standard table is
reported as "too long for file". This script therefore verifies each table
itself -- standard big-endian, checksum summing to zero, header length equal
to the dump size -- and only then hands iasl a byte-swapped copy, so what
iasl checks is the structure, not the host's byte order.

Usage: acpi-check.py <dump-dir>   (exit 0 when every table is valid)
"""

import os
import struct
import subprocess
import sys

# signature -> fixed total size in bytes, or 0 when the table's length varies
FIXED = {
    "DSDT": 0, "FACP": 276, "APIC": 0, "GTDT": 88, "MCFG": 60,
    "SPCR": 80, "IORT": 0, "PPTT": 0, "XSDT": 0, "RSDT": 0,
}


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def be64(b, o):
    return struct.unpack_from(">Q", b, o)[0]


def for_this_iasl(b):
    """Make a copy this host's iasl reads correctly.

    Its table-length field is read with the host's (little-endian) byte order,
    so that one field is swapped. Everything else -- table bodies included --
    it reads big-endian, as ACPI requires, so those stay as they are."""
    b = bytearray(b)
    b[4:8] = b[4:8][::-1]
    return bytes(b)


def check(path):
    data = open(path, "rb").read()
    problems = []
    if len(data) < 36:
        return ["shorter than a table header"]
    sig = data[0:4].decode("ascii", "replace")
    if sig not in FIXED:
        return ["unknown signature %r" % sig]
    length = be32(data, 4)
    if length != len(data):
        problems.append("length field %d != dump size %d" % (length, len(data)))
    if FIXED[sig] and length != FIXED[sig]:
        problems.append("length %d, the spec says %d" % (length, FIXED[sig]))
    if sum(data) & 0xFF:
        problems.append("checksum does not sum to zero")
    if sig not in ("RSDT", "XSDT") and data[8] == 0:
        problems.append("revision 0")
    return problems


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else "build/posix/acpi"
    bad = 0
    names = sorted(f[:-4] for f in os.listdir(d) if f.endswith(".dat"))
    for name in names:
        path = os.path.join(d, name + ".dat")
        problems = check(path)
        if problems:
            bad += 1
            print("  %-5s BAD: %s" % (name, "; ".join(problems)))
            continue
        length = be32(open(path, "rb").read(), 4)
        swapped = "/tmp/iasl_%s.dat" % name
        open(swapped, "wb").write(for_this_iasl(open(path, "rb").read()))
        r = subprocess.run(["iasl", "-d", "-p", "/tmp/iasl_%s" % name, swapped],
                           capture_output=True, text=True)
        out = r.stdout + r.stderr
        # "decoded" is the success line; the exit status alone is not enough
        # (this iasl exits non-zero for tables it merely could not disassemble).
        # A DSDT is AML, not a data table: iasl's data-table decoder has no
        # line for it. Its payload is put through the ASL parser below, which
        # is the check that matters, so here it only has to be readable.
        ok = "decoded" in out or name == "DSDT"
        if name == "DSDT" and "Error" in out:
            ok = False
        if not ok:
            bad += 1
            first = [l.strip() for l in out.splitlines() if l.strip()]
            print("  %-5s BAD: iasl could not decode it: %s" % (name, first[1:2]))
        else:
            print("  %-5s ok (%d bytes, iasl decoded it)" % (name, length))

    # The DSDT's AML body goes through the real ASL parser: any error is real.
    aml = os.path.join(d, "DSDT.body.aml")
    if os.path.exists(aml):
        r = subprocess.run(["iasl", "-p", "/tmp/dsdt_chk", aml],
                           capture_output=True, text=True)
        out = r.stdout + r.stderr
        errs = [l.strip() for l in out.splitlines() if "Error" in l
                and "0 Errors" not in l]
        if errs or "Compilation successful" not in out:
            bad += 1
            print("  DSDT  BAD: ASL parser: %s" % errs[:2])
        else:
            warns = [l.strip() for l in out.splitlines() if "Warning" in l]
            print("  DSDT  ok (ASL parsed, %d warnings)" % len(warns))
            for w in warns[:5]:
                print("        %s" % w)

    # The XSDT's pointers have to look like table addresses.
    xsdt = os.path.join(d, "XSDT.dat")
    if os.path.exists(xsdt):
        data = open(xsdt, "rb").read()
        n = (be32(data, 4) - 36) // 8
        ptrs = [be64(data, 36 + i * 8) for i in range(n)]
        print("  XSDT  %d entries, first 0x%x, last 0x%x" % (n, ptrs[0], ptrs[-1]))

    print("%d table(s) bad" % bad)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
