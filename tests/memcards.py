#!/usr/bin/env python3
"""PS2 memory cards and Dreamcast VMUs for the e2e test, holding a given
number of saves: enough of each format for src/memcard.c to tell a blank
card from one with saves on it.

  memcards.py ps2 OUT SAVES TAG    an 8 MB PS2 card with ECC, as PCSX2 makes them
  memcards.py vmu OUT SAVES TAG    a 128 KB VMU
TAG goes into the card's data, so two cards with the same count still differ.
"""
import struct
import sys


def ps2(saves, tag):
    page, ppc, clusters, alloc, root = 512, 2, 8192, 41, 0
    stride = page + 16
    card = bytearray(b"\xff" * (clusters * ppc * stride))
    sb = bytearray(0x154)
    sb[0:28] = b"Sony PS2 Memory Card Format "
    sb[28:40] = b"1.2.0.0\0\0\0\0\0"
    struct.pack_into("<HHHH", sb, 0x28, page, ppc, 16, 0xFF00)
    struct.pack_into("<IIII", sb, 0x30, clusters, alloc, clusters - 1, root)
    card[0:len(sb)] = sb
    # The root directory's "." entry: exists and a directory, its entry count.
    at = (alloc + root) * ppc * stride
    card[at:at + 512] = bytes(512)
    struct.pack_into("<HHI", card, at, 0x8427, 0, saves + 2)
    data = (alloc + 100) * ppc * stride
    card[data:data + len(tag)] = tag.encode()
    return bytes(card)


def vmu(saves, tag):
    card = bytearray(128 * 1024)
    root = 255 * 512
    card[root:root + 16] = b"\x55" * 16
    struct.pack_into("<HH", card, root + 0x4A, 253, 13)
    for i in range(saves):
        card[253 * 512 + i * 32] = 0x33  # a data file
    card[0:len(tag)] = tag.encode()
    return bytes(card)


def main():
    kind, out, saves, tag = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
    with open(out, "wb") as f:
        f.write({"ps2": ps2, "vmu": vmu}[kind](saves, tag))


if __name__ == "__main__":
    main()
