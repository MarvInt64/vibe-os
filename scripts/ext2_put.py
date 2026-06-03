#!/usr/bin/env python3
"""Inject a host file into VibeOS's ext2 disk image under a target path.

Mirrors the kernel's simple ext2 layout (block_size=1024, 256-byte inodes,
direct blocks plus one single-indirect block). The disk must already be formatted by the kernel
(boot once with `make run`). Usage:

    ext2_put.py <disk.img> <host_file> </bin/name>
"""
import sys, struct, os

BS = 1024                # block size
INODE_SIZE = 256
EXT2_MAGIC = 0xEF53
S_IFREG = 0o100000
S_IFDIR = 0o040000

def die(msg):
    sys.stderr.write("ext2_put: " + msg + "\n")
    sys.exit(1)

class Ext2:
    def __init__(self, path):
        self.f = open(path, "r+b")
        sb = self.read_block(1)
        (self.inodes_count, self.blocks_count, _resv, self.free_blocks,
         self.free_inodes, self.first_data_block, self.log_block_size) = struct.unpack_from("<7I", sb, 0)
        magic = struct.unpack_from("<H", sb, 0x38)[0]
        if magic != EXT2_MAGIC:
            die("disk not formatted (magic=0x%x). Run 'make run' once to format it." % magic)
        if (1024 << self.log_block_size) != BS:
            die("unexpected block size")
        # Block bitmap spans as many consecutive blocks as needed (starting at
        # block 2) to cover all blocks_count bits.  Inode bitmap follows.
        bb_blocks = (self.blocks_count + 8 * BS - 1) // (8 * BS)
        self.bb_start  = 2
        self.ib_start  = 2 + bb_blocks
        self.it_start  = self.ib_start + 1   # inode table starts here
        self.block_bitmap = bytearray()
        for i in range(bb_blocks):
            self.block_bitmap += bytearray(self.read_block(self.bb_start + i))
        self.inode_bitmap = bytearray(self.read_block(self.ib_start))

    def read_block(self, n):
        self.f.seek(n * BS)
        return self.f.read(BS)

    def write_block(self, n, data):
        assert len(data) <= BS
        self.f.seek(n * BS)
        self.f.write(data.ljust(BS, b"\0"))

    # ---- inode access ----
    def inode_loc(self, ino):
        idx = ino - 1
        blk = self.it_start + (idx * INODE_SIZE) // BS
        off = (idx * INODE_SIZE) % BS
        return blk, off

    def read_inode(self, ino):
        blk, off = self.inode_loc(ino)
        return bytearray(self.read_block(blk)[off:off + INODE_SIZE])

    def write_inode(self, ino, data):
        blk, off = self.inode_loc(ino)
        b = bytearray(self.read_block(blk))
        b[off:off + INODE_SIZE] = data.ljust(INODE_SIZE, b"\0")[:INODE_SIZE]
        self.write_block(blk, bytes(b))

    @staticmethod
    def inode_mode(node):
        return struct.unpack_from("<H", node, 0)[0]

    @staticmethod
    def inode_block(node, i):
        return struct.unpack_from("<I", node, 40 + i * 4)[0]

    # ---- bitmaps ----
    def alloc_block(self):
        for i in range(self.first_data_block, self.blocks_count):
            if not (self.block_bitmap[i >> 3] & (1 << (i & 7))):
                self.block_bitmap[i >> 3] |= (1 << (i & 7))
                self.free_blocks -= 1
                return i
        die("no free blocks")

    def free_block(self, b):
        if b <= 0 or b >= self.blocks_count:
            return
        if self.block_bitmap[b >> 3] & (1 << (b & 7)):
            self.block_bitmap[b >> 3] &= ~(1 << (b & 7))
            self.free_blocks += 1

    # Free all data + indirect blocks an existing inode references (direct,
    # single, double indirect), so overwriting a file does not leak blocks.
    def free_inode_blocks(self, node):
        import struct as _s
        PTRS = BS // 4
        ib = [_s.unpack_from("<I", node, 40 + i * 4)[0] for i in range(15)]
        for i in range(12):
            self.free_block(ib[i])
        if ib[12]:
            t = self.read_block(ib[12])
            for k in range(PTRS):
                self.free_block(_s.unpack_from("<I", t, k * 4)[0])
            self.free_block(ib[12])
        if ib[13]:
            l1 = self.read_block(ib[13])
            for g in range(PTRS):
                l2b = _s.unpack_from("<I", l1, g * 4)[0]
                if not l2b:
                    continue
                l2 = self.read_block(l2b)
                for k in range(PTRS):
                    self.free_block(_s.unpack_from("<I", l2, k * 4)[0])
                self.free_block(l2b)
            self.free_block(ib[13])

    def alloc_inode(self):
        for i in range(11, min(self.inodes_count, BS * 8)):
            if not (self.inode_bitmap[i >> 3] & (1 << (i & 7))):
                self.inode_bitmap[i >> 3] |= (1 << (i & 7))
                self.free_inodes -= 1
                return i + 1  # 1-based
        die("no free inodes")

    # ---- directory ----
    def dir_find(self, dir_ino, name):
        node = self.read_inode(dir_ino)
        for i in range(12):
            b = self.inode_block(node, i)
            if b == 0:
                continue
            blk = self.read_block(b)
            off = 0
            while off < BS:
                ino, rec_len, name_len, _ft = struct.unpack_from("<IHBB", blk, off)
                if rec_len == 0:
                    break
                if ino != 0 and blk[off + 8:off + 8 + name_len] == name.encode():
                    return ino
                off += rec_len
        return 0

    def dir_add(self, dir_ino, new_ino, name, file_type):
        name_b = name.encode()
        needed = (8 + len(name_b) + 3) & ~3
        node = self.read_inode(dir_ino)
        # Try existing direct blocks first.
        for i in range(12):
            b = self.inode_block(node, i)
            if b == 0:
                break
            blk = bytearray(self.read_block(b))
            off = 0
            while off < BS:
                ino, rec_len, name_len, ft = struct.unpack_from("<IHBB", blk, off)
                if rec_len == 0:
                    break
                used = (8 + name_len + 3) & ~3 if ino != 0 else 0
                if rec_len >= used + needed:
                    new_off = off + used
                    if ino != 0:
                        struct.pack_into("<H", blk, off + 4, used)
                    new_rec = rec_len - used
                    struct.pack_into("<IHBB", blk, new_off, new_ino, new_rec, len(name_b), file_type)
                    blk[new_off + 8:new_off + 8 + len(name_b)] = name_b
                    self.write_block(b, bytes(blk))
                    return True
                off += rec_len
        # All existing blocks full — allocate a new direct block (up to block[11]).
        for i in range(12):
            b = self.inode_block(node, i)
            if b == 0:
                nb = self.alloc_block()
                if nb is None:
                    die("no free blocks while extending directory")
                new_blk = bytearray(BS)
                struct.pack_into("<IHBB", new_blk, 0, new_ino, BS, len(name_b), file_type)
                new_blk[8:8 + len(name_b)] = name_b
                self.write_block(nb, bytes(new_blk))
                ib = bytearray(self.read_inode(dir_ino))
                struct.pack_into("<I", ib, 40 + i * 4, nb)
                node_size = struct.unpack_from("<I", ib, 4)[0]
                struct.pack_into("<I", ib, 4, node_size + BS)
                blks = struct.unpack_from("<I", ib, 28)[0]
                struct.pack_into("<I", ib, 28, blks + BS // 512)
                self.write_inode(dir_ino, bytes(ib))
                return True
        die("no room in directory (all 12 direct blocks full)")

    def mkdir(self, parent_ino, name):
        """Create an empty directory `name` under parent_ino; return its inode."""
        ino = self.alloc_inode()
        blk = self.alloc_block()
        # Directory data block: '.' then '..' (which spans the rest of the block).
        buf = bytearray(BS)
        struct.pack_into("<IHBB", buf, 0, ino, 12, 1, 2)        # '.'
        buf[8:9] = b"."
        struct.pack_into("<IHBB", buf, 12, parent_ino, BS - 12, 2, 2)  # '..'
        buf[20:22] = b".."
        self.write_block(blk, bytes(buf))
        # Inode: dir mode, one block of data, link count 2 (itself + '.').
        node = bytearray(INODE_SIZE)
        struct.pack_into("<H", node, 0, S_IFDIR | 0o755)
        struct.pack_into("<I", node, 4, BS)
        struct.pack_into("<H", node, 26, 2)
        struct.pack_into("<I", node, 28, BS // 512)
        struct.pack_into("<I", node, 40, blk)
        self.write_inode(ino, node)
        # Link into parent and bump the parent's link count for the new '..'.
        self.dir_add(parent_ino, ino, name, 2)   # file_type 2 = directory
        pnode = self.read_inode(parent_ino)
        pl = struct.unpack_from("<H", pnode, 26)[0]
        struct.pack_into("<H", pnode, 26, pl + 1)
        self.write_inode(parent_ino, pnode)
        return ino

    def flush_meta(self):
        # write block bitmap back (may span multiple blocks)
        for i in range(0, len(self.block_bitmap), BS):
            self.write_block(self.bb_start + i // BS, bytes(self.block_bitmap[i:i+BS]))
        # write inode bitmap
        self.write_block(self.ib_start, bytes(self.inode_bitmap))
        # update free counts in superblock
        sb = bytearray(self.read_block(1))
        struct.pack_into("<I", sb, 0x0C, self.free_blocks & 0xffffffff)
        struct.pack_into("<I", sb, 0x10, self.free_inodes & 0xffffffff)
        self.write_block(1, bytes(sb))

def main():
    if len(sys.argv) != 4:
        die("usage: ext2_put.py <disk.img> <host_file> </abs/path>")
    img, host_file, target = sys.argv[1], sys.argv[2], sys.argv[3]
    if not target.startswith("/"):
        die("target must be absolute, e.g. /bin/uidemo")
    data = open(host_file, "rb").read()
    nblocks = (len(data) + BS - 1) // BS
    PTRS = BS // 4
    if nblocks > 12 + PTRS + PTRS * PTRS:
        die("file too big (%d blocks); direct + single + double indirect only" % nblocks)

    parent = target.rsplit("/", 1)[0] or "/"
    name = target.rsplit("/", 1)[1]
    fs = Ext2(img)

    # resolve parent directory inode, creating any missing components
    dir_ino = 2  # root
    if parent != "/":
        for comp in parent.strip("/").split("/"):
            nxt = fs.dir_find(dir_ino, comp)
            if nxt == 0:
                nxt = fs.mkdir(dir_ino, comp)
            dir_ino = nxt

    # remove existing entry's inode reuse? Simple approach: if exists, overwrite
    existing = fs.dir_find(dir_ino, name)
    if existing:
        ino = existing
        node = fs.read_inode(ino)
        fs.free_inode_blocks(node)   # release old data blocks (no leak on rebuild)
    else:
        ino = fs.alloc_inode()
        node = bytearray(INODE_SIZE)

    # allocate data blocks and write content
    blocks = [fs.alloc_block() for _ in range(nblocks)]
    for i, b in enumerate(blocks):
        fs.write_block(b, data[i * BS:(i + 1) * BS])

    iblock = [0] * 15
    meta_blocks = 0
    for i in range(12):
        iblock[i] = blocks[i] if i < nblocks else 0

    # single indirect: data blocks 12 .. 12+PTRS-1
    if nblocks > 12:
        si = fs.alloc_block(); meta_blocks += 1
        buf = bytearray(BS)
        for k, b in enumerate(blocks[12:12 + PTRS]):
            struct.pack_into("<I", buf, k * 4, b)
        fs.write_block(si, bytes(buf))
        iblock[12] = si

    # double indirect: block[13] -> L1 table -> L2 tables -> data
    if nblocks > 12 + PTRS:
        rest = blocks[12 + PTRS:]
        l1 = fs.alloc_block(); meta_blocks += 1
        l1buf = bytearray(BS)
        ngroups = (len(rest) + PTRS - 1) // PTRS
        for g in range(ngroups):
            l2 = fs.alloc_block(); meta_blocks += 1
            l2buf = bytearray(BS)
            for k, b in enumerate(rest[g * PTRS:(g + 1) * PTRS]):
                struct.pack_into("<I", l2buf, k * 4, b)
            fs.write_block(l2, bytes(l2buf))
            struct.pack_into("<I", l1buf, g * 4, l2)
        fs.write_block(l1, bytes(l1buf))
        iblock[13] = l1

    # build inode
    struct.pack_into("<H", node, 0, S_IFREG | 0o755)   # mode
    struct.pack_into("<I", node, 4, len(data))          # size
    struct.pack_into("<H", node, 26, 1)                 # links_count
    allocated_blocks = nblocks + meta_blocks
    struct.pack_into("<I", node, 28, allocated_blocks * (BS // 512))  # blocks (512-units)
    for i in range(15):
        struct.pack_into("<I", node, 40 + i * 4, iblock[i])
    fs.write_inode(ino, node)

    if not existing:
        fs.dir_add(dir_ino, ino, name, 1)  # file_type 1 = regular

    fs.flush_meta()
    fs.f.close()
    print("ext2_put: installed %s (%d bytes, %d blocks, inode %d)" % (target, len(data), nblocks, ino))

if __name__ == "__main__":
    main()
