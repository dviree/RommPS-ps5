"""xemu hard disk images for the tests, written and read without the code
under test: qcow2 (version 3, 64 KiB clusters) holding a FATX E: drive.

  xboxhdd.py make <image> [<title id> <save folder>] [--compressed]
      a blank disk, or one with E:\\UDATA\\<title id> holding the folder's
      files; --compressed stores E:'s first clusters deflated, as blank disks
      from qemu-img convert -c come
  xboxhdd.py get <image> <title id> <out dir>
      copies E:\\UDATA\\<title id> out as <out dir>/<its name>/...; prints
      "none" when there's no such save
  xboxhdd.py check <image>
      every cluster's refcount against what uses it; prints "ok" or exits 1
"""
import os
import struct
import sys
import zlib

CBITS = 16
CS = 1 << CBITS
SIZE = 8 << 30
E_OFFSET, E_SIZE = 0xABE80000, 0x1312D6000
FCS = 16384  # FATX cluster: 32 sectors
NCLUS = E_SIZE // FCS
FAT_SIZE = (NCLUS * 4 + 0xFFF) & ~0xFFF
DATA = E_OFFSET + 0x1000 + FAT_SIZE
END = 0xFFFFFFFF


def dirent(name, is_dir, first, size):
    e = struct.pack("<BB", len(name), 0x10 if is_dir else 0) + name.encode() + b"\xff" * (42 - len(name))
    t = 0x35498138  # 2026-10-09 16:09:48
    return e + struct.pack("<IIIII", first, size, t, t, t)


def make(path, title=None, folder=None, compressed=False):
    guest = {}  # guest cluster index -> bytearray

    def gw(off, data):
        while data:
            ci, within = divmod(off, CS)
            take = min(len(data), CS - within)
            guest.setdefault(ci, bytearray(CS))[within:within + take] = data[:take]
            off, data = off + take, data[take:]

    fat = {0: 0xFFFFFFF8, 1: END}
    nxt = [2]

    def alloc(n):
        cl = list(range(nxt[0], nxt[0] + n))
        nxt[0] += n
        for a, b in zip(cl, cl[1:] + [None]):
            fat[a] = b if b else END
        return cl

    def put_dir(entries):
        n = max(1, -(-len(entries) * 64 // FCS))
        cl = alloc(n)
        buf = bytearray(b"\xff" * (n * FCS))
        for i, e in enumerate(entries):
            buf[i * 64:i * 64 + 64] = e
        for k, c in enumerate(cl):
            gw(DATA + (c - 1) * FCS, bytes(buf[k * FCS:(k + 1) * FCS]))
        return cl[0]

    def put_tree(src):
        entries = []
        for name in sorted(os.listdir(src)):
            p = os.path.join(src, name)
            if os.path.isdir(p):
                entries.append(dirent(name, True, put_tree(p), 0))
                continue
            data = open(p, "rb").read()
            cl = alloc(-(-len(data) // FCS)) if data else [0]
            for k, c in enumerate(cl if data else []):
                gw(DATA + (c - 1) * FCS, data[k * FCS:(k + 1) * FCS])
            entries.append(dirent(name, False, cl[0], len(data)))
        return put_dir(entries)

    root = []
    if title:
        save = put_tree(folder)
        root.append(dirent("UDATA", True, put_dir([dirent(title.lower(), True, save, 0)]), 0))
    rb = bytearray(b"\xff" * FCS)
    for i, e in enumerate(root):
        rb[i * 64:i * 64 + 64] = e
    gw(DATA, bytes(rb))  # the root is cluster 1
    gw(E_OFFSET, b"FATX" + struct.pack("<III", 0x1234, 32, 1) + b"\x00\x00")
    fb = bytearray(FAT_SIZE)
    for c, v in fat.items():
        struct.pack_into("<I", fb, c * 4, v)
    gw(E_OFFSET + 0x1000, bytes(fb))

    # The file: header, refcount table, refcount block, L1, then the L2 tables and data.
    host = [None, None, None, None]
    l1 = [0] * 16
    l2 = {}
    deflated = {E_OFFSET >> CBITS} if compressed else set()
    packs = bytearray()
    for ci in sorted(guest):
        t = ci // (CS // 8)
        if t not in l2:
            l2[t] = [0] * (CS // 8)
            l1[t] = len(host) * CS | 1 << 63
            host.append(("l2", t))
        if ci in deflated:
            co = zlib.compressobj(9, zlib.DEFLATED, -12)
            z = co.compress(bytes(guest[ci])) + co.flush()
            l2[t][ci % (CS // 8)] = ("z", len(packs), len(z))
            packs += z + b"\x00" * (-len(z) % 512)
        else:
            l2[t][ci % (CS // 8)] = len(host) * CS | 1 << 63
            host.append(("data", ci))
    zbase = len(host) * CS
    nz = -(-len(packs) // CS)
    refs = [1] * len(host) + [0] * nz
    xb = 62 - (CBITS - 8)
    for t in l2:
        for k, v in enumerate(l2[t]):
            if isinstance(v, tuple):
                off = zbase + v[1]
                sectors = -(-((off % 512) + v[2]) // 512) - 1
                l2[t][k] = 1 << 62 | sectors << xb | off
                for c in range(off >> CBITS, ((off + (sectors + 1) * 512 - 1) >> CBITS) + 1):
                    refs[c] += 1
    out = bytearray(len(host) * CS) + packs + b"\x00" * (nz * CS - len(packs))
    hdr = struct.pack(">4sIQIIQIIQQIIQQQQII", b"QFI\xfb", 3, 0, 0, CBITS, SIZE, 0, 16, 3 * CS, 1 * CS, 1, 0, 0, 0, 0, 0, 4, 104)
    out[0:len(hdr)] = hdr
    struct.pack_into(">Q", out, CS, 2 * CS)
    for i, r in enumerate(refs):
        struct.pack_into(">H", out, 2 * CS + i * 2, r)
    for i, v in enumerate(l1):
        struct.pack_into(">Q", out, 3 * CS + i * 8, v)
    for i, h in enumerate(host):
        if h and h[0] == "l2":
            for k, v in enumerate(l2[h[1]]):
                struct.pack_into(">Q", out, i * CS + k * 8, v)
        elif h and h[0] == "data":
            out[i * CS:(i + 1) * CS] = guest[h[1]]
    open(path, "wb").write(out)


class Disk:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(104)
        self.cbits = struct.unpack(">I", h[20:24])[0]
        self.cs = 1 << self.cbits
        l1n, l1o = struct.unpack(">IQ", h[36:48])
        self.f.seek(l1o)
        self.l1 = struct.unpack(">%dQ" % l1n, self.f.read(8 * l1n))

    def entry(self, ci):
        per = self.cs // 8
        o = self.l1[ci // per] & 0x00FFFFFFFFFFFE00
        if not o:
            return 0
        self.f.seek(o + ci % per * 8)
        return struct.unpack(">Q", self.f.read(8))[0]

    def cluster(self, ci):
        e = self.entry(ci)
        if e >> 62 & 1:
            xb = 62 - (self.cbits - 8)
            off = e & ((1 << xb) - 1)
            self.f.seek(off)
            return zlib.decompressobj(-15).decompress(self.f.read(((e >> xb) & 0xFF) * 512 + 512))[:self.cs]
        o = e & 0x00FFFFFFFFFFFE00
        if not o or e & 1:
            return bytes(self.cs)
        self.f.seek(o)
        return self.f.read(self.cs)

    def read(self, off, n):
        out = b""
        while n:
            ci, within = divmod(off, self.cs)
            take = min(n, self.cs - within)
            out += self.cluster(ci)[within:within + take]
            off, n = off + take, n - take
        return out


def get(path, title, out):
    d = Disk(path)
    fat = d.read(E_OFFSET + 0x1000, FAT_SIZE)

    def chain(c):
        while c and c < 0xFFFFFFF0:
            yield c
            c = struct.unpack_from("<I", fat, c * 4)[0]

    def read_chain(c, size=None):
        b = b"".join(d.read(DATA + (x - 1) * FCS, FCS) for x in chain(c))
        return b if size is None else b[:size]

    def ls(c):
        b = read_chain(c)
        for i in range(0, len(b), 64):
            n = b[i]
            if n in (0, 0xFF):
                break
            if n == 0xE5:
                continue
            first, size = struct.unpack_from("<II", b, i + 44)
            yield b[i + 2:i + 2 + n].decode(), b[i + 1] & 0x10, first, size

    def find(c, name):
        return next((e for e in ls(c) if e[0].lower() == name.lower()), None)

    def copy(c, to):
        os.makedirs(to, exist_ok=True)
        for name, is_dir, first, size in ls(c):
            if is_dir:
                copy(first, os.path.join(to, name))
            else:
                open(os.path.join(to, name), "wb").write(read_chain(first, size) if size else b"")

    u = find(1, "UDATA")
    s = find(u[2], title) if u else None
    if not s:
        print("none")
        return
    copy(s[2], os.path.join(out, s[0]))


def check(path):
    f = open(path, "rb")
    size = os.path.getsize(path)
    h = f.read(104)
    cbits = struct.unpack(">I", h[20:24])[0]
    cs = 1 << cbits
    l1n, l1o, rto, rtc = struct.unpack(">IQQI", h[36:60])
    want = {}

    def use(off, n=1):
        for k in range(n):
            want[(off >> cbits) + k] = want.get((off >> cbits) + k, 0) + 1

    use(0)
    use(l1o, -(-l1n * 8 // cs))
    use(rto, rtc)
    f.seek(l1o)
    for e in struct.unpack(">%dQ" % l1n, f.read(8 * l1n)):
        o = e & 0x00FFFFFFFFFFFE00
        if not o:
            continue
        use(o)
        f.seek(o)
        for x in struct.unpack(">%dQ" % (cs // 8), f.read(cs)):
            if x >> 62 & 1:
                xb = 62 - (cbits - 8)
                off = x & ((1 << xb) - 1)
                end = (off & ~511) + (((x >> xb) & ((1 << (62 - xb)) - 1)) + 1) * 512
                for c in range(off >> cbits, ((end - 1) >> cbits) + 1):
                    want[c] = want.get(c, 0) + 1
            elif x & 0x00FFFFFFFFFFFE00:
                use(x & 0x00FFFFFFFFFFFE00)
    f.seek(rto)
    have = {}
    per = cs // 2
    for i, b in enumerate(struct.unpack(">%dQ" % (rtc * cs // 8), f.read(rtc * cs))):
        if not b:
            continue
        use(b)
        f.seek(b)
        blk = f.read(cs)
        for j in range(per):
            v = struct.unpack_from(">H", blk, j * 2)[0]
            if v:
                have[i * per + j] = v
    bad = sorted(c for c in set(want) | set(have) if want.get(c, 0) != have.get(c, 0))
    past = [c for c in want if (c + 1) * cs > size]
    if bad or past:
        sys.exit("refcounts wrong for clusters %s, past the end %s" % (bad[:8], past[:8]))
    print("ok")


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[0] == "make":
        z = "--compressed" in a
        a = [x for x in a if x != "--compressed"]
        make(a[1], *(a[2:4] if len(a) >= 4 else []), compressed=z)
    elif a[0] == "get":
        get(a[1], a[2], a[3])
    elif a[0] == "check":
        check(a[1])
    else:
        sys.exit(__doc__)
