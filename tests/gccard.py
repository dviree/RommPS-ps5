#!/usr/bin/env python3
"""GameCube memory cards for the e2e test, written apart from src/gcmem.c so
each checks the other.

  gccard.py blank CARD NBLOCKS              a formatted, empty card
  gccard.py gci OUT ID FILE BLOCKS FILL     a .gci: ID "GZLE01", FILE its name on the card
  gccard.py put CARD GCI                    puts a .gci on the card
  gccard.py saves CARD ID                   "<file name> <md5 of the save's blocks>" per save
  gccard.py check CARD                      exits 1 unless the card is one Dolphin reads
"""
import hashlib
import struct
import sys

B = 0x2000


def be16(b, o):
    return struct.unpack_from(">H", b, o)[0]


def sums(b):
    a = c = 0
    for i in range(0, len(b), 2):
        w = be16(b, i)
        a = (a + w) & 0xFFFF
        c = (c + (w ^ 0xFFFF)) & 0xFFFF
    return (0 if a == 0xFFFF else a), (0 if c == 0xFFFF else c)


def seal(card, blk, is_dir):
    o = blk * B
    a, c = sums(card[o:o + 0x1FFC]) if is_dir else sums(card[o + 4:o + B])
    struct.pack_into(">HH", card, o + (0x1FFC if is_dir else 0), a, c)


def current(card):
    def ok(blk, is_dir):
        o = blk * B
        a, c = sums(card[o:o + 0x1FFC]) if is_dir else sums(card[o + 4:o + B])
        return (a, c) == struct.unpack_from(">HH", card, o + (0x1FFC if is_dir else 0))

    def newer(x, y, at):
        cx, cy = ok(x, at == 0x1FFA), ok(y, at == 0x1FFA)
        if cx and cy:
            sx, sy = (struct.unpack_from(">h", card, z * B + at)[0] for z in (x, y))
            return y if sy > sx else x
        return x if cx else y if cy else None

    return newer(1, 2, 0x1FFA), newer(3, 4, 4)


def entries(card, d):
    for i in range(127):
        o = d * B + i * 0x40
        if card[o:o + 4] != b"\xff" * 4:
            yield i, o


def chain(card, t, first, count):
    blk, out = first, []
    for _ in range(count):
        out.append(blk)
        blk = be16(card, t * B + 0xA + (blk - 5) * 2)
    return out


def blank(nblocks):
    card = bytearray(b"\xff" * (nblocks * B))
    card[0:0x26] = bytes(0x26)
    struct.pack_into(">H", card, 0x22, nblocks // 16)
    a, c = sums(card[0:0x1FC])
    struct.pack_into(">HH", card, 0x1FC, a, c)
    for k in (1, 2):
        struct.pack_into(">H", card, k * B + 0x1FFA, k - 1)
        seal(card, k, True)
    for k in (3, 4):
        card[k * B:(k + 1) * B] = bytes(B)
        struct.pack_into(">HHH", card, k * B + 4, k - 3, nblocks - 5, 4)
        seal(card, k, False)
    return card


def put(card, g):
    """Like Dolphin's import: the same save is dropped, new blocks follow the
    last one handed out, and the edited copies replace the older ones."""
    d, t = current(card)
    nd, nt = (2 if d == 1 else 1), (4 if t == 3 else 3)
    card[nd * B:(nd + 1) * B] = card[d * B:(d + 1) * B]
    card[nt * B:(nt + 1) * B] = card[t * B:(t + 1) * B]
    nblocks = len(card) // B
    for i, o in list(entries(card, nd)):
        if card[o:o + 6] == g[0:6] and card[o + 8:o + 40] == g[8:40]:
            for blk in chain(card, nt, be16(card, o + 0x36), be16(card, o + 0x38)):
                struct.pack_into(">H", card, nt * B + 0xA + (blk - 5) * 2, 0)
            struct.pack_into(">H", card, nt * B + 6, be16(card, nt * B + 6) + be16(card, o + 0x38))
            card[o:o + 0x40] = b"\xff" * 0x40
    count = be16(g, 0x38)
    start = be16(card, nt * B + 8) + 1
    if not 5 <= start < nblocks:
        start = 5
    free = [b for b in (5 + (start - 5 + k) % (nblocks - 5) for k in range(nblocks - 5))
            if be16(card, nt * B + 0xA + (b - 5) * 2) == 0][:count]
    slot = next(i for i in range(127) if card[nd * B + i * 0x40:nd * B + i * 0x40 + 4] == b"\xff" * 4)
    for k, blk in enumerate(free):
        card[blk * B:(blk + 1) * B] = g[0x40 + k * B:0x40 + (k + 1) * B]
        struct.pack_into(">H", card, nt * B + 0xA + (blk - 5) * 2, free[k + 1] if k + 1 < count else 0xFFFF)
    struct.pack_into(">H", card, nt * B + 6, be16(card, nt * B + 6) - count)
    struct.pack_into(">H", card, nt * B + 8, free[-1])
    o = nd * B + slot * 0x40
    card[o:o + 0x40] = g[0:0x40]
    struct.pack_into(">H", card, o + 0x36, free[0])
    for blk, at in ((nd, 0x1FFA), (nt, 4)):
        struct.pack_into(">H", card, blk * B + at, (be16(card, blk * B + at) + 1) & 0xFFFF)
    seal(card, nd, True)
    seal(card, nt, False)


def saves(card, gid):
    d, t = current(card)
    for i, o in entries(card, d):
        if card[o:o + 6] == gid.encode():
            data = b"".join(card[b * B:(b + 1) * B] for b in chain(card, t, be16(card, o + 0x36), be16(card, o + 0x38)))
            yield card[o + 8:o + 40].rstrip(b"\0").decode(), hashlib.md5(data).hexdigest()


def check(card):
    d, t = current(card)
    assert d and t, "no directory or table checks out"
    nblocks, used = len(card) // B, set()
    for i, o in entries(card, d):
        blocks = chain(card, t, be16(card, o + 0x36), be16(card, o + 0x38))
        assert all(5 <= b < nblocks for b in blocks), "a chain leaves the card"
        assert not used & set(blocks), "a block in two saves"
        used |= set(blocks)
    zeros = sum(1 for b in range(5, nblocks) if be16(card, t * B + 0xA + (b - 5) * 2) == 0)
    assert zeros == be16(card, t * B + 6) == nblocks - 5 - len(used), "free count is off"


if __name__ == "__main__":
    cmd, a = sys.argv[1], sys.argv[2:]
    if cmd == "blank":
        open(a[0], "wb").write(blank(int(a[1])))
    elif cmd == "gci":
        gid, name, blocks, fill = a[1].encode(), a[2].encode(), int(a[3]), int(a[4], 0)
        head = bytearray(b"\xff" * 0x40)
        head[0:6], head[8:40] = gid, name.ljust(32, b"\0")
        struct.pack_into(">HH", head, 0x36, 0, blocks)
        open(a[0], "wb").write(bytes(head) + b"".join(bytes([(fill + k) & 0xFF]) * B for k in range(blocks)))
    elif cmd == "put":
        card = bytearray(open(a[0], "rb").read())
        put(card, open(a[1], "rb").read())
        open(a[0], "wb").write(card)
    elif cmd == "saves":
        for name, md5 in saves(bytearray(open(a[0], "rb").read()), a[1]):
            print(name, md5)
    elif cmd == "check":
        check(bytearray(open(a[0], "rb").read()))
