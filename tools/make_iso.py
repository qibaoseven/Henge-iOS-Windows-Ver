#!/usr/bin/env python3
"""A small ISO9660 writer: just enough to put files on a CD that Windows reads.

Why this exists: the driver disc has to be built reproducibly wherever the app
is built, and pulling in xorriso/genisoimage for it means another tool to install
on every machine -- including the one that builds in CI. ISO9660 level 1 is a
simple enough format to write directly.

No Joliet, no Rock Ridge, no boot record: names are upper-case 8.3, which is
exactly what cdfs.sys shows and what install.bat expects. One extent per file,
no multi-extent (a file bigger than 4 GiB would need it -- this writer refuses
instead of writing something Windows would mount as garbage).

  tools/make_iso.py <source-dir> <out.iso> [--volume-id HENGEDRV] [--verify]
"""
import os
import sys

BLOCK = 2048

LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"


def iso_name(name, is_dir):
    """8.3 upper case. Directories get no extension."""
    name = name.upper()
    base, ext = name, ""
    if not is_dir and "." in name:
        base, ext = name.rsplit(".", 1)
    base = "".join(c if c in LETTERS else "_" for c in base)[:8] or "_"
    if is_dir:
        return base
    ext = "".join(c for c in ext if c in LETTERS)[:3]
    return base + ("." + ext if ext else "")


class Entry:
    def __init__(self, path, name, is_dir):
        self.path = path          # full host path
        self.name = name          # host name
        self.iso = iso_name(name, is_dir)
        self.dir = is_dir
        self.children = []
        self.blocks = 0           # extent length in bytes (dirs) or file size
        self.lba = 0
        self.parent = None

    @property
    def iso_record_name(self):
        return self.iso + ("" if self.dir else ";1")


def scan(root):
    def walk(path, name, is_dir):
        e = Entry(path, name, is_dir)
        if is_dir:
            for child in sorted(os.listdir(path), key=lambda n: n.upper()):
                p = os.path.join(path, child)
                try:
                    is_sub = os.path.isdir(p)
                    size = 0 if is_sub else os.path.getsize(p)
                except OSError:
                    continue
                if not is_sub and size > 0xFFFFFFFF:
                    raise SystemExit(f"error: {p} is larger than 4 GiB; this writer has no multi-extent")
                e.children.append(walk(p, child, is_sub))
        else:
            e.blocks = os.path.getsize(path)
        return e

    root_entry = walk(root, "", True)
    root_entry.iso = ""  # the root's own name is the volume, not a directory entry
    return root_entry


def assign_dir_sizes(root):
    """Record lengths depend only on names, so sizes can be fixed before any LBA."""
    order = []

    def visit(e, depth):
        order.append(e)
        for c in e.children:
            if c.dir:
                c.parent = e
                visit(c, depth + 1)

    visit(root, 0)

    for e in order:
        size = 34 + len(bytes([0]))  # "." record, name length 1 -> 34
        size += 34                    # ".." record
        for c in e.children:
            size += 33 + len(c.iso_record_name)
            size += (len(c.iso_record_name) + 33) % 2  # records are even
        # A directory extent must end on a block boundary and records may not
        # straddle blocks, so pad generously: one record per 2048-byte block
        # would waste space, but computing exact block packing is not worth it
        # for a disc of a few hundred files.
        e.blocks = ((size + BLOCK - 1) // BLOCK) * BLOCK
    return order


def path_table_entries(order):
    """(name, parent_index, extent) for the little- and big-endian path tables."""
    index = {id(e): i + 1 for i, e in enumerate(order)}
    rows = []
    for e in order:
        name = "" if e.parent is None and e is order[0] else e.iso
        parent = index.get(id(e.parent), 1) if e.parent else 1
        rows.append((name, parent, e.lba))
    return rows


def le32(v):
    v &= 0xFFFFFFFF
    return bytes([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF])


def be32(v):
    v &= 0xFFFFFFFF
    return bytes([(v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF])


def le16(v):
    v &= 0xFFFF
    return bytes([v & 0xFF, (v >> 8) & 0xFF])


def be16(v):
    v &= 0xFFFF
    return bytes([(v >> 8) & 0xFF, v & 0xFF])


def both32(v):
    v &= 0xFFFFFFFF
    return bytes([v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF,
                  (v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF])


def both16(v):
    v &= 0xFFFF
    return bytes([v & 0xFF, (v >> 8) & 0xFF, (v >> 8) & 0xFF, v & 0xFF])


def dirrecord(extent, size, name_bytes, is_dir):
    """ECMA-119 9.1: 33 bytes of fields plus the name, padded to even."""
    body = bytearray()
    body += bytes([0])          # extended attribute record length: none
    body += both32(extent)
    body += both32(size)
    body += b"\x00" * 7        # recording date and time
    body += bytes([0x02 if is_dir else 0x00])   # file flags
    body += bytes([0])          # file unit size
    body += bytes([0])          # interleave gap size
    body += both16(1)           # volume sequence number
    body += bytes([len(name_bytes)])
    body += name_bytes
    body += b"\x00" * ((len(name_bytes) + 33) % 2)  # pad to even
    return bytes([1 + len(body)]) + bytes(body)


def build_dir_block(e, self_lba, parent_lba):
    recs = bytearray()
    recs += dirrecord(self_lba, e.blocks, b"\x00", True)
    recs += dirrecord(parent_lba, e.blocks, b"\x01", True)
    for c in e.children:
        recs += dirrecord(c.lba, c.blocks, c.iso_record_name.encode("ascii"), c.dir)
    if len(recs) > e.blocks:
        raise SystemExit(f"error: directory {e.iso} needs {len(recs)} bytes, allotted {e.blocks}")
    return bytes(recs)


def pad(b, to=BLOCK):
    n = (-len(b)) % to
    return b + b"\x00" * n


def write_iso(root, out, volume_id):
    order = assign_dir_sizes(root)

    # Layout after the 16-block system area:
    #   16 PVD, 17 terminator, 18 L path table, 19 M path table,
    #   then directory extents, then file data.
    pvd_lba, term_lba = 16, 17
    lpt_lba = 18
    pt_len = 0
    for name, _, _ in path_table_entries(order):
        pt_len += 8 + len(name) + (len(name) % 2)
    pt_blocks = (pt_len + BLOCK - 1) // BLOCK
    mpt_lba = lpt_lba + pt_blocks

    next_lba = mpt_lba + pt_blocks
    for e in order:
        e.lba = next_lba
        next_lba += e.blocks // BLOCK
    for e in order:
        for c in e.children:
            if not c.dir:
                c.lba = next_lba
                next_lba += (c.blocks + BLOCK - 1) // BLOCK
    total_blocks = next_lba

    # Path tables need the final LBAs, so they are written after assignment.
    lt = bytearray()
    mt = bytearray()
    for name, parent, lba in path_table_entries(order):
        name_b = name.encode("ascii")
        lt += bytes([len(name), 0]) + le32(lba) + le16(parent) + name_b
        mt += bytes([len(name), 0]) + be32(lba) + be16(parent) + name_b
        if len(name) % 2:
            lt += b"\x00"
            mt += b"\x00"

    def field(b, off, length, value):
        assert len(value) <= length, f"PVD field at {off} is {length} bytes"
        b[off:off + length] = value.ljust(length)

    pvd = bytearray(b"\x00" * BLOCK)
    pvd[0] = 1
    field(pvd, 1, 5, b"CD001")
    pvd[6] = 1                                  # version
    field(pvd, 8, 32, b"")                      # system identifier
    field(pvd, 40, 32, volume_id.encode("ascii"))
    pvd[80:88] = both32(total_blocks)           # volume space size
    pvd[120:124] = both16(1)                    # volume set size
    pvd[124:128] = both16(1)                    # volume sequence number
    pvd[128:132] = both16(BLOCK)                # logical block size
    pvd[132:140] = both32(pt_blocks * BLOCK)    # path table size (padded to blocks)
    pvd[140:144] = le32(lpt_lba)                # type L path table, little endian
    pvd[148:152] = be32(mpt_lba)                # type M path table, big endian
    root_record = dirrecord(root.lba, root.blocks, b"\x00", True)
    pvd[156:156 + len(root_record)] = root_record
    field(pvd, 156 + 34, 128, b"")              # volume set identifier
    field(pvd, 318, 128, b"")                   # publisher
    field(pvd, 446, 128, b"")                   # data preparer
    field(pvd, 574, 128, b"HENGE ISO WRITER")   # application identifier
    field(pvd, 702, 37, b"")                    # copyright file
    field(pvd, 739, 37, b"")                    # abstract file
    field(pvd, 776, 37, b"")                    # bibliographic file
    for off in (813, 830, 847, 864):            # creation/modification/expiration/effective
        field(pvd, off, 17, b"2020010100000000\x00")
    pvd[881] = 1                                 # file structure version

    term = bytearray(b"\x00" * BLOCK)
    term[0:8] = b"\xFFCD001\x01\x00"

    def checksum(block):
        # ISO9660 puts no checksum in the PVD; nothing to do.
        return block

    out_f = open(out, "wb")
    out_f.write(b"\x00" * (16 * BLOCK))
    out_f.write(checksum(bytes(pvd)))
    out_f.write(bytes(term))
    out_f.write(pad(bytes(lt)))
    out_f.write(pad(bytes(mt)))

    for e in order:
        parent_lba = e.parent.lba if e.parent else root.lba
        out_f.write(pad(build_dir_block(e, e.lba, parent_lba)))

    for e in order:
        for c in e.children:
            if not c.dir:
                out_f.write(open(c.path, "rb").read())
                out_f.write(b"\x00" * ((-c.blocks) % BLOCK))
    out_f.close()
    return total_blocks


def verify(iso, root):
    """Read the root directory back: the only real check that the layout is sane."""
    with open(iso, "rb") as f:
        f.seek(16 * BLOCK)
        pvd = f.read(BLOCK)
        assert pvd[0] == 1 and pvd[1:6] == b"CD001" and pvd[6] == 1, "no PVD at block 16"
        root_rec = pvd[156:190]
        lba = int.from_bytes(root_rec[2:6], "little")
        size = int.from_bytes(root_rec[10:14], "little")
        f.seek(lba * BLOCK)
        data = f.read(size)
    names, i = [], 0
    while i < len(data) and data[i]:
        rec_len = data[i]
        if rec_len == 0:
            break
        name_len = data[i + 32]
        raw = data[i + 33:i + 33 + name_len]
        if raw not in (b"\x00", b"\x01"):
            names.append(raw.decode("ascii"))
        i += rec_len
    expected = sorted(c.iso_record_name for c in root.children)
    got = sorted(names)
    if got != expected:
        raise SystemExit(f"verify failed: root lists {got}, expected {expected}")
    return len(names)


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    flags = {a for a in argv[1:] if a.startswith("--")}
    if len(args) < 2:
        print(__doc__)
        return 2
    src, out = args[0], args[1]
    volume = "HENGEDRV"
    for i, a in enumerate(argv):
        if a == "--volume-id":
            volume = argv[i + 1]

    parent = os.path.dirname(os.path.abspath(out))
    os.makedirs(parent, exist_ok=True)

    root = scan(src)
    blocks = write_iso(root, out, volume)
    size = os.path.getsize(out)
    if "--verify" in flags:
        n = verify(out, root)
        print(f"{out}: {size} bytes, {blocks} blocks, {n} top-level entries, verified")
    else:
        print(f"{out}: {size} bytes, {blocks} blocks")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
