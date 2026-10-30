#!/usr/bin/env python3
"""Tiny disc images and PARAM.SFO files for tests.

    make_disc.py psp <out.iso> <DISC_ID>
    make_disc.py sfo <out.sfo> KEY=VALUE ...
    make_disc.py cso <out.cso> <DISC_ID>
    make_disc.py gc <out.iso> <GAMEID>
    make_disc.py psx <out.iso> <BOOT EXE>     a PS1 ISO whose SYSTEM.CNF boots it ("SLUS_123.45")
    make_disc.py psxbin <out.bin> <BOOT EXE>  the same as raw 2352-byte mode 2 sectors
    make_disc.py ps2 <out.iso> <BOOT ELF> [DIR]  a PS2 ISO whose SYSTEM.CNF BOOT2 is the ELF (in DIR when given);
                                                 prints PCSX2's CRC of it
"""
import struct
import sys

SECTOR = 2048


def sfo(values):
    keys = list(values)
    key_table = b""
    key_offsets = []
    for k in keys:
        key_offsets.append(len(key_table))
        key_table += k.encode() + b"\0"
    key_table += b"\0" * (-len(key_table) % 4)
    data = b""
    index = b""
    for i, k in enumerate(keys):
        v = values[k].encode() + b"\0"
        size = len(v) + (-len(v) % 4)
        index += struct.pack("<HHIII", key_offsets[i], 0x0204, len(v), size, len(data))
        data += v + b"\0" * (size - len(v))
    key_start = 20 + len(index)
    data_start = key_start + len(key_table)
    return b"\0PSF" + struct.pack("<IIII", 0x0101, key_start, data_start, len(keys)) + index + key_table + data


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def record(name, lba, size, is_dir):
    rec = bytes([0, 0]) + both32(lba) + both32(size) + bytes(7) + bytes([2 if is_dir else 0, 0, 0]) + both16(1)
    rec += bytes([len(name)]) + name
    if len(rec) % 2:
        rec += b"\0"
    return bytes([len(rec)]) + rec[1:]


def psp_iso(disc_id):
    param = sfo({"DISC_ID": disc_id, "TITLE": "Test Game"})
    img = bytearray(SECTOR * 21)
    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[156:156 + 34] = record(b"\0", 18, SECTOR, True)
    img[16 * SECTOR:17 * SECTOR] = pvd
    img[17 * SECTOR:17 * SECTOR + 7] = b"\xffCD001\x01"
    root = record(b"\0", 18, SECTOR, True) + record(b"\1", 18, SECTOR, True) + record(b"PSP_GAME", 19, SECTOR, True)
    img[18 * SECTOR:18 * SECTOR + len(root)] = root
    game = record(b"\0", 19, SECTOR, True) + record(b"\1", 18, SECTOR, True) + record(b"PARAM.SFO;1", 20, len(param), False)
    img[19 * SECTOR:19 * SECTOR + len(game)] = game
    img[20 * SECTOR:20 * SECTOR + len(param)] = param
    return bytes(img)


def psx_iso(boot):
    cnf = f"BOOT = cdrom:\\{boot};1\r\nTCB = 4\r\nEVENT = 10\r\nSTACK = 801FFFF0\r\n".encode()
    img = bytearray(SECTOR * 20)
    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[156:156 + 34] = record(b"\0", 18, SECTOR, True)
    img[16 * SECTOR:17 * SECTOR] = pvd
    img[17 * SECTOR:17 * SECTOR + 7] = b"\xffCD001\x01"
    root = record(b"\0", 18, SECTOR, True) + record(b"\1", 18, SECTOR, True) + record(b"SYSTEM.CNF;1", 19, len(cnf), False)
    img[18 * SECTOR:18 * SECTOR + len(root)] = root
    img[19 * SECTOR:19 * SECTOR + len(cnf)] = cnf
    return bytes(img)


def ps2_elf():
    """A boot program over two sectors, its length not a multiple of 4 (PCSX2 skips the tail)."""
    return bytes((i * 7 + (i >> 8)) & 0xFF for i in range(SECTOR * 2 + 6))


def ps2_iso(boot, folder=None):
    elf = ps2_elf()
    path = f"\\{folder}\\{boot};1" if folder else f"\\{boot};1"
    cnf = f"BOOT2 = cdrom0:{path}\r\nVER = 1.00\r\nVMODE = NTSC\r\n".encode()
    img = bytearray(SECTOR * 26)
    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[156:156 + 34] = record(b"\0", 18, SECTOR, True)
    img[16 * SECTOR:17 * SECTOR] = pvd
    img[17 * SECTOR:17 * SECTOR + 7] = b"\xffCD001\x01"
    root = record(b"\0", 18, SECTOR, True) + record(b"\1", 18, SECTOR, True) + record(b"SYSTEM.CNF;1", 19, len(cnf), False)
    if folder:
        root += record(folder.encode(), 20, SECTOR, True)
        sub = record(b"\0", 20, SECTOR, True) + record(b"\1", 18, SECTOR, True) + record(f"{boot};1".encode(), 21, len(elf), False)
        img[20 * SECTOR:20 * SECTOR + len(sub)] = sub
    else:
        root += record(f"{boot};1".encode(), 21, len(elf), False)
    img[18 * SECTOR:18 * SECTOR + len(root)] = root
    img[19 * SECTOR:19 * SECTOR + len(cnf)] = cnf
    img[21 * SECTOR:21 * SECTOR + len(elf)] = elf
    crc = 0
    for (w,) in struct.iter_unpack("<I", elf[:len(elf) // 4 * 4]):
        crc ^= w
    return bytes(img), crc


def raw_mode2(iso):
    """2048-byte sectors as 2352-byte mode 2 form 1 ones (sync, header, subheader, data, no EDC/ECC)."""
    out = bytearray()
    for i in range(0, len(iso), SECTOR):
        out += b"\0" + b"\xff" * 10 + b"\0" + bytes(3) + b"\x02" + bytes(8) + iso[i:i + SECTOR] + bytes(280)
    return bytes(out)


def cso(iso, block=2048):
    import zlib
    blocks = [iso[i:i + block] for i in range(0, len(iso), block)]
    header = b"CISO" + struct.pack("<IQIBBH", 24, len(iso), block, 1, 0, 0)
    body, index = b"", []
    offset = len(header) + 4 * (len(blocks) + 1)
    for i, b in enumerate(blocks):
        index.append(offset + len(body))
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        z = c.compress(b) + c.flush()
        if i % 2 or len(z) >= len(b):  # store some blocks uncompressed
            index[-1] |= 0x80000000
            z = b
        body += z
    index.append(offset + len(body))
    return header + b"".join(struct.pack("<I", x) for x in index) + body


def xbe(title_id):
    """A default.xbe down to what matters: a certificate holding the title id."""
    base = 0x10000
    head = bytearray(0x200)
    head[0:4] = b"XBEH"
    struct.pack_into("<I", head, 0x104, base)
    struct.pack_into("<I", head, 0x118, base + 0x200)
    cert = struct.pack("<III", 0x1D0, 0, title_id) + bytes(0x1D0 - 12)
    return bytes(head) + cert


def xex(title_id):
    """A default.xex down to its optional headers: one to skip, then the
    execution info holding the title id."""
    head = b"XEX2" + struct.pack(">IIIII", 0, 0x1000, 0, 0x100, 2)
    head += struct.pack(">II", 0x000183FF, 0) + struct.pack(">II", 0x00040006, 0x40)
    head += bytes(0x40 - len(head))
    return head + struct.pack(">IIII", 0x12345678, 0, 0, title_id) + bytes(8 + 0x100)


def xiso(title_id, boot=b"default.xbe", exe=None):
    """A trimmed Xbox (or 360) disc: XDVDFS with the boot file a right child
    of the root entry, so finding it walks the tree."""
    def entry(left, right, sector, size, attr, name):
        e = struct.pack("<HHIIBB", left, right, sector, size, attr, len(name)) + name
        return e + b"\xff" * (-len(e) % 4)
    exe = exe or xbe(title_id)
    first = entry(0, 0, 40, 4, 0, b"AUDIO.BIN")
    table = entry(0, len(first) // 4, 40, 4, 0, b"AUDIO.BIN")
    table += entry(0, 0, 41, len(exe), 0, boot)
    vd = bytearray(SECTOR)
    vd[0:20] = vd[0x7EC:0x800] = b"MICROSOFT*XBOX*MEDIA"
    struct.pack_into("<II", vd, 0x14, 33, len(table))
    disc = bytearray(41 * SECTOR)
    disc[0x10000:0x10800] = vd
    disc[33 * SECTOR:33 * SECTOR + len(table)] = table
    return bytes(disc) + exe


if __name__ == "__main__":
    kind, out = sys.argv[1], sys.argv[2]
    if kind == "psp":
        data = psp_iso(sys.argv[3])
    elif kind == "cso":
        data = cso(psp_iso(sys.argv[3]))
    elif kind == "sfo":
        data = sfo(dict(a.split("=", 1) for a in sys.argv[3:]))
    elif kind == "psx":
        data = psx_iso(sys.argv[3])
    elif kind == "psxbin":
        data = raw_mode2(psx_iso(sys.argv[3]))
    elif kind == "ps2":
        data, crc = ps2_iso(sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else None)
        print("%08X" % crc)
    elif kind == "gc":
        data = sys.argv[3].encode()[:6] + bytes(0x20) + b"\xc2\x33\x9f\x3d" + bytes(64)
    elif kind == "xiso":
        data = xiso(int(sys.argv[3], 16))
    elif kind == "xbe":
        data = xbe(int(sys.argv[3], 16))
    elif kind == "x360iso":
        data = xiso(0, b"default.xex", xex(int(sys.argv[3], 16)))
    elif kind == "god":  # a GOD package's header file: the title id at 0x360
        h = bytearray(0xB000)
        h[0:4] = b"LIVE"
        struct.pack_into(">I", h, 0x344, 0x7000)
        struct.pack_into(">I", h, 0x360, int(sys.argv[3], 16))
        data = bytes(h)
    elif kind == "xex":
        data = xex(int(sys.argv[3], 16))
    else:
        sys.exit("unknown kind")
    open(out, "wb").write(data)
