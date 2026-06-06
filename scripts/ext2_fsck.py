#!/usr/bin/env python3
"""Consistency checker / repairer for VibeOS's simple ext2 disk image.

VibeOS's filesystem is written by two independent minimal ext2 implementations
(scripts/ext2_put.py on the host, kernel/src/ext2_fs.c in the guest). Mixing
host writes with guest writes across boots, or many repeated overwrites, can let
their bitmap / free-count bookkeeping drift and eventually double-allocate a
block, which silently corrupts a directory or file. This tool walks the image
the way fsck does and reports — or with --repair fixes — those inconsistencies.

Layout mirrors ext2_put.py: block_size=1024, 256-byte inodes, block bitmap at
block 2.., inode bitmap next, then the inode table. Block N occupies bitmap bit
N; inode N occupies inode-bitmap bit N-1.

Usage:
    ext2_fsck.py <disk.img>            # check only, exit 1 if problems found
    ext2_fsck.py <disk.img> --repair   # rebuild bitmaps + free counts from the
                                        # inode/dir tree, then re-check
"""
import sys, struct

BS = 1024
INODE_SIZE = 256
EXT2_MAGIC = 0xEF53
PTRS = BS // 4
ROOT_INO = 2
S_IFMT = 0o170000
S_IFDIR = 0o040000
S_IFREG = 0o100000


class Img:
    def __init__(self, path, writable):
        self.f = open(path, "r+b" if writable else "rb")
        sb = self.rd(1)
        (self.inodes_count, self.blocks_count, _r, self.free_blocks,
         self.free_inodes, self.first_data_block, self.log_bs) = struct.unpack_from("<7I", sb, 0)
        if struct.unpack_from("<H", sb, 0x38)[0] != EXT2_MAGIC:
            sys.exit("ext2_fsck: not an ext2 image (bad magic)")
        if (1024 << self.log_bs) != BS:
            sys.exit("ext2_fsck: unexpected block size")
        self.bb_blocks = (self.blocks_count + 8 * BS - 1) // (8 * BS)
        self.bb_start = 2
        self.ib_start = 2 + self.bb_blocks
        self.it_start = self.ib_start + 1
        self.inode_table_blocks = (self.inodes_count * INODE_SIZE + BS - 1) // BS
        self.first_data = self.it_start + self.inode_table_blocks
        self.block_bitmap = bytearray()
        for i in range(self.bb_blocks):
            self.block_bitmap += bytearray(self.rd(self.bb_start + i))
        self.inode_bitmap = bytearray(self.rd(self.ib_start))

    def rd(self, n):
        self.f.seek(n * BS)
        d = self.f.read(BS)
        return d if len(d) == BS else d.ljust(BS, b"\0")

    def wr(self, n, data):
        self.f.seek(n * BS)
        self.f.write(bytes(data).ljust(BS, b"\0")[:BS])

    def inode(self, ino):
        idx = ino - 1
        blk = self.it_start + (idx * INODE_SIZE) // BS
        off = (idx * INODE_SIZE) % BS
        return bytearray(self.rd(blk)[off:off + INODE_SIZE])

    def block_used(self, b):
        return bool(self.block_bitmap[b >> 3] & (1 << (b & 7)))

    def inode_used(self, ino):
        i = ino - 1
        return bool(self.inode_bitmap[i >> 3] & (1 << (i & 7)))


def inode_blocks(img, node):
    """Yield (block, kind) for every disk block an inode references.
    kind is 'data' or 'meta' (indirect tables)."""
    ib = [struct.unpack_from("<I", node, 40 + i * 4)[0] for i in range(15)]
    for i in range(12):
        if ib[i]:
            yield ib[i], 'data'
    if ib[12]:
        yield ib[12], 'meta'
        t = img.rd(ib[12])
        for k in range(PTRS):
            b = struct.unpack_from("<I", t, k * 4)[0]
            if b:
                yield b, 'data'
    if ib[13]:
        yield ib[13], 'meta'
        l1 = img.rd(ib[13])
        for g in range(PTRS):
            l2b = struct.unpack_from("<I", l1, g * 4)[0]
            if not l2b:
                continue
            yield l2b, 'meta'
            l2 = img.rd(l2b)
            for k in range(PTRS):
                b = struct.unpack_from("<I", l2, k * 4)[0]
                if b:
                    yield b, 'data'


def dir_entries(img, node):
    """Yield (ino, name, file_type, block, off, rec_len) for a directory inode.
    Stops a block's walk on a malformed rec_len (reported by caller)."""
    for i in range(12):
        b = struct.unpack_from("<I", node, 40 + i * 4)[0]
        if not b:
            continue
        blk = img.rd(b)
        off = 0
        while off + 8 <= BS:
            ino, rec_len, name_len, ft = struct.unpack_from("<IHBB", blk, off)
            if rec_len < 8 or off + rec_len > BS:
                yield ('BAD', None, 0, b, off, rec_len)
                break
            name = blk[off + 8:off + 8 + name_len].decode('latin1') if ino else ''
            yield (ino, name, ft, b, off, rec_len)
            off += rec_len


def metadata_blocks(img):
    s = set([0, 1])
    for i in range(img.bb_blocks):
        s.add(img.bb_start + i)
    s.add(img.ib_start)
    for i in range(img.inode_table_blocks):
        s.add(img.it_start + i)
    return s


def check(img, repair):
    problems = []
    meta = metadata_blocks(img)

    # ---- walk every in-use inode, collect block ownership ----
    owner = {}          # block -> list of inodes referencing it
    referenced = set()
    used_inodes = [ino for ino in range(1, img.inodes_count + 1) if img.inode_used(ino)]
    # inodes 1..10 are reserved; root(2) is the tree anchor
    for ino in used_inodes:
        if ino < ROOT_INO:
            continue
        node = img.inode(ino)
        mode = struct.unpack_from("<H", node, 0)[0]
        if mode == 0:
            problems.append("inode %d marked used but mode==0 (dangling)" % ino)
            continue
        for b, _kind in inode_blocks(img, node):
            if b >= img.blocks_count:
                problems.append("inode %d references out-of-range block %d" % (ino, b))
                continue
            owner.setdefault(b, []).append(ino)
            referenced.add(b)

    # ---- double-allocated blocks (the corruption that bites) ----
    for b, owners in sorted(owner.items()):
        if len(owners) > 1:
            problems.append("block %d double-allocated by inodes %s" % (b, owners))
        if b in meta:
            problems.append("block %d referenced by inode(s) %s but is filesystem metadata" % (b, owners))

    # ---- bitmap vs reality ----
    should_used = set(meta) | referenced
    bitmap_used = set(b for b in range(img.blocks_count) if img.block_used(b))
    under = sorted(should_used - bitmap_used)      # in use but bitmap says free -> will be reused
    leaked = sorted(bitmap_used - should_used)     # bitmap says used but nobody owns it -> leak
    for b in under:
        problems.append("block %d in use but FREE in bitmap (double-alloc risk)" % b)
    if leaked:
        problems.append("%d blocks marked used but unreferenced (leaked): %s%s" %
                        (len(leaked), leaked[:12], " ..." if len(leaked) > 12 else ""))

    # ---- directory tree: reachability, rec_len, '.'/'..' ----
    reachable = set([ROOT_INO])
    stack = [ROOT_INO]
    seen_dirs = set()
    while stack:
        d = stack.pop()
        if d in seen_dirs:
            continue
        seen_dirs.add(d)
        node = img.inode(d)
        names = {}
        has_dot = has_dotdot = False
        for ino, name, ft, blk, off, rec_len in dir_entries(img, node):
            if ino == 'BAD':
                problems.append("dir inode %d: malformed entry at block %d off %d (rec_len=%d) — truncates listing"
                                % (d, blk, off, rec_len))
                continue
            if ino == 0:
                continue
            if name == '.':
                has_dot = True; continue
            if name == '..':
                has_dotdot = True; continue
            if name in names:
                problems.append("dir inode %d: duplicate entry '%s' (inodes %d and %d)"
                                % (d, name, names[name], ino))
            names[name] = ino
            if ino < 1 or ino > img.inodes_count:
                problems.append("dir inode %d: entry '%s' -> invalid inode %d" % (d, name, ino))
                continue
            if not img.inode_used(ino):
                problems.append("dir inode %d: entry '%s' -> inode %d not marked used" % (d, name, ino))
            reachable.add(ino)
            cmode = struct.unpack_from("<H", img.inode(ino), 0)[0]
            if (cmode & S_IFMT) == S_IFDIR:
                stack.append(ino)
        if not has_dot or not has_dotdot:
            problems.append("dir inode %d: missing %s" % (d, "'.'" if not has_dot else "'..'"))

    orphans = sorted(set(i for i in used_inodes if i >= 11) - reachable)
    if orphans:
        problems.append("%d in-use inode(s) unreachable from root (orphaned/leaked): %s%s" %
                        (len(orphans), orphans[:12], " ..." if len(orphans) > 12 else ""))

    # ---- free counts ----
    real_free_blocks = img.blocks_count - len(bitmap_used)
    real_free_inodes = img.inodes_count - sum(1 for i in range(1, img.inodes_count + 1) if img.inode_used(i))
    if img.free_blocks != real_free_blocks:
        problems.append("superblock free_blocks=%d but bitmap shows %d" % (img.free_blocks, real_free_blocks))
    if img.free_inodes != real_free_inodes:
        problems.append("superblock free_inodes=%d but bitmap shows %d" % (img.free_inodes, real_free_inodes))

    # ---- report ----
    if not problems:
        print("ext2_fsck: clean (%d blocks, %d inodes, %d/%d free)"
              % (img.blocks_count, img.inodes_count, real_free_blocks, real_free_inodes))
        return 0
    print("ext2_fsck: %d problem(s) found:" % len(problems))
    for p in problems:
        print("  - " + p)

    if not repair:
        print("Run with --repair to rebuild bitmaps and free counts from the inode/dir tree.")
        return 1

    # ---- repair: rebuild bitmaps from reachable tree, fix counts ----
    print("ext2_fsck: repairing...")
    # Keep only inodes reachable from root (plus root); drop orphans.
    keep_inodes = set([ROOT_INO]) | reachable
    new_ibm = bytearray(len(img.inode_bitmap))
    for ino in keep_inodes:
        new_ibm[(ino - 1) >> 3] |= 1 << ((ino - 1) & 7)
    # Recompute referenced blocks from the kept inodes only.
    new_ref = set(meta)
    for ino in sorted(keep_inodes):
        if ino < ROOT_INO:
            continue
        node = img.inode(ino)
        if struct.unpack_from("<H", node, 0)[0] == 0:
            continue
        for b, _k in inode_blocks(img, node):
            if b < img.blocks_count:
                new_ref.add(b)
    new_bbm = bytearray(len(img.block_bitmap))
    for b in new_ref:
        new_bbm[b >> 3] |= 1 << (b & 7)
    img.block_bitmap = new_bbm
    img.inode_bitmap = new_ibm
    # write bitmaps
    for i in range(img.bb_blocks):
        img.wr(img.bb_start + i, img.block_bitmap[i * BS:(i + 1) * BS])
    img.wr(img.ib_start, img.inode_bitmap)
    # fix free counts in superblock
    img.free_blocks = img.blocks_count - len(new_ref)
    img.free_inodes = img.inodes_count - len(keep_inodes)
    sb = bytearray(img.rd(1))
    struct.pack_into("<I", sb, 0x0C, img.free_blocks & 0xffffffff)
    struct.pack_into("<I", sb, 0x10, img.free_inodes & 0xffffffff)
    img.wr(1, sb)
    img.f.flush()
    print("ext2_fsck: rebuilt bitmaps; free_blocks=%d free_inodes=%d. Re-checking..."
          % (img.free_blocks, img.free_inodes))
    # reload and re-check (note: directory rec_len corruption is NOT auto-fixed)
    return 0


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    repair = "--repair" in sys.argv
    if len(args) != 1:
        sys.exit("usage: ext2_fsck.py <disk.img> [--repair]")
    img = Img(args[0], writable=repair)
    rc = check(img, repair)
    if repair:
        # fresh pass over the repaired image to confirm
        img2 = Img(args[0], writable=False)
        rc = check(img2, False)
    sys.exit(rc)


if __name__ == "__main__":
    main()
