#!/usr/bin/env python3
# Copyright (c) 2026 Natalia Portillo <claunia@claunia.com>
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read files from a Be File System (BFS) volume, read-only.

usage: bfs.py IMAGE[@OFFSET] ls PATH
       bfs.py IMAGE[@OFFSET] get PATH OUTFILE

OFFSET is where the volume starts in IMAGE, in bytes; a number ending
in "s" counts 2048-byte CD blocks (the BeOS R4 CD's PowerPC volume is
image.iso@160186s).
"""

import os
import stat
import struct
import sys

BFS_MAGIC1 = 0x42465331
BFS_MAGIC2 = 0xDD121031
BFS_MAGIC3 = 0x15B6830E
INODE_MAGIC = 0x3BBE0AD9
BTREE_MAGIC = 0x69F6C2E8
BTREE_NULL = -1


class BFS:
    def __init__(self, path, offset):
        self.f = open(path, "rb")
        self.base = offset
        # PowerPC volumes have the superblock at 0, x86 ones at 512
        for at in (0, 512):
            sb = self.read(at, 512)
            if struct.unpack_from(">I", sb, 32)[0] == BFS_MAGIC1:
                self.e = ">"
                break
            if struct.unpack_from("<I", sb, 32)[0] == BFS_MAGIC1:
                self.e = "<"
                break
        else:
            sys.exit("no BFS superblock at offset %d" % offset)
        e = self.e
        self.name = sb[:32].split(b"\0")[0].decode("latin-1")
        (self.block_size, self.block_shift, _, _, self.inode_size,
         magic2, _, self.ag_shift) = struct.unpack_from(e + "IIqqiIii", sb, 40)
        if magic2 != BFS_MAGIC2:
            sys.exit("bad BFS superblock")
        self.root = self.run(sb, 116)

    def read(self, pos, length):
        self.f.seek(self.base + pos)
        return self.f.read(length)

    def run(self, buf, off):
        ag, start, length = struct.unpack_from(self.e + "iHH", buf, off)
        return ((ag << self.ag_shift) + start, length)

    def inode(self, block):
        buf = self.read(block * self.block_size, self.inode_size)
        if struct.unpack_from(self.e + "i", buf, 0)[0] != INODE_MAGIC:
            sys.exit("bad inode at block %d" % block)
        return buf

    def runs(self, ino):
        """The block runs of an inode's data stream, in order."""
        e = self.e
        out = []
        for i in range(12):
            blk, n = self.run(ino, 72 + i * 8)
            if n:
                out.append((blk, n))
        iblk, ilen = self.run(ino, 176)
        if ilen:
            data = self.read(iblk * self.block_size, ilen * self.block_size)
            for i in range(0, len(data), 8):
                blk, n = self.run(data, i)
                if n:
                    out.append((blk, n))
        dblk, dlen = self.run(ino, 192)
        if dlen:
            sys.exit("double-indirect files are not handled")
        return out

    def data(self, ino):
        size = struct.unpack_from(self.e + "q", ino, 208)[0]
        parts = []
        for blk, n in self.runs(ino):
            parts.append(self.read(blk * self.block_size, n * self.block_size))
        return b"".join(parts)[:size]

    def entries(self, ino):
        """Name and inode block of each directory entry, from its B+tree."""
        e = self.e
        tree = self.data(ino)
        magic, node_size = struct.unpack_from(e + "Ii", tree, 0)
        if magic != BTREE_MAGIC:
            sys.exit("bad B+tree")
        node = struct.unpack_from(e + "q", tree, 16)[0]
        # Go down the leftmost children to the first leaf
        while True:
            left, right, overflow, count, keylen = \
                struct.unpack_from(e + "qqqHH", tree, node)
            if overflow == BTREE_NULL:
                break
            ends, values = self.node_arrays(tree, node, count, keylen)
            node = values[0] if count else overflow
        while node != BTREE_NULL:
            left, right, overflow, count, keylen = \
                struct.unpack_from(e + "qqqHH", tree, node)
            ends, values = self.node_arrays(tree, node, count, keylen)
            start = 0
            for i in range(count):
                key = tree[node + 28 + start:node + 28 + ends[i]]
                start = ends[i]
                yield key.decode("utf-8", "replace"), values[i]
            node = right

    def node_arrays(self, tree, node, count, keylen):
        e = self.e
        off = node + 28 + keylen
        off = (off + 7) & ~7
        ends = struct.unpack_from(e + "%dH" % count, tree, off)
        off += 2 * count
        values = struct.unpack_from(e + "%dq" % count, tree, off)
        return ends, values

    def lookup(self, path):
        block = self.root[0]
        for part in [p for p in path.split("/") if p]:
            for name, value in self.entries(self.inode(block)):
                if name == part:
                    block = value
                    break
            else:
                sys.exit("%s: not found" % path)
        return self.inode(block)


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    image, _, off = sys.argv[1].partition("@")
    if off.endswith("s"):
        offset = int(off[:-1]) * 2048
    else:
        offset = int(off or "0", 0)
    fs = BFS(image, offset)
    cmd, path = sys.argv[2], sys.argv[3]
    ino = fs.lookup(path)
    mode = struct.unpack_from(fs.e + "i", ino, 20)[0]
    if cmd == "ls":
        if not stat.S_ISDIR(mode):
            print(path, struct.unpack_from(fs.e + "q", ino, 208)[0])
            return
        for name, block in fs.entries(ino):
            if name in (".", ".."):
                continue
            child = fs.inode(block)
            cmode = struct.unpack_from(fs.e + "i", child, 20)[0]
            size = struct.unpack_from(fs.e + "q", child, 208)[0]
            print("%s %10d %s" % ("d" if stat.S_ISDIR(cmode) else "-",
                                  size, name))
    elif cmd == "get" and len(sys.argv) == 5:
        with open(sys.argv[4], "wb") as out:
            out.write(fs.data(ino))
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
