#!/usr/bin/env python3
"""Format a blank disk image with VibeOS's custom ext2 layout.

Mirrors ext2_format() in kernel/src/ext2_fs.c exactly so ext2_put.py and the
kernel can both use it without a first-boot cycle.

Usage: format_disk.py <disk.img>
"""
import sys, struct, os

BS         = 1024
INODE_SIZE = 256
EXT2_MAGIC = 0xEF53
EXT2_ROOT  = 2

def die(msg):
    sys.stderr.write("format_disk: " + msg + "\n"); sys.exit(1)

def main():
    if len(sys.argv) != 2:
        die("usage: format_disk.py <disk.img>")
    path = sys.argv[1]
    disk_size = os.path.getsize(path)
    if disk_size < 2 * BS:
        die("disk image too small")

    blocks_count   = disk_size // BS
    inodes_count   = min(1024, blocks_count // 4)
    inode_tbl_size = inodes_count * INODE_SIZE
    inode_tbl_blks = (inode_tbl_size + BS - 1) // BS
    # Block bitmap spans enough blocks to cover all blocks_count bits.
    # Block 2+  = block bitmap;  next block = inode bitmap;  then inode table.
    bb_blocks  = (blocks_count + 8 * BS - 1) // (8 * BS)
    ib_block   = 2 + bb_blocks         # inode bitmap block
    it_start   = ib_block + 1          # inode table starts here
    first_data = it_start + inode_tbl_blks

    # ---- block bitmap (bb_blocks × 1024 bytes) ----
    bm = bytearray(bb_blocks * BS)
    for i in range(first_data):
        bm[i // 8] |= 1 << (i % 8)
    free_blocks = blocks_count - first_data

    # ---- inode bitmap ----
    ibm = bytearray((inodes_count + 7) // 8)
    # Mark inode 2 (root) used; inode indices are 0-based in the bitmap
    ibm[(EXT2_ROOT - 1) // 8] |= 1 << ((EXT2_ROOT - 1) % 8)
    free_inodes = inodes_count - 1

    # ---- inode table (zeroed) ----
    itbl = bytearray(inode_tbl_size)
    # Root inode at index 1 (inode 2 = index 1)
    # mode = S_IFDIR | 0755 = 0o040755 = 0x41ED
    root_off = (EXT2_ROOT - 1) * INODE_SIZE
    root_blk = first_data          # allocate first available block
    struct.pack_into("<HHI", itbl, root_off + 0, 0x41ED, 0, 0)  # mode, uid, size
    # size = BS (one block)
    struct.pack_into("<I", itbl, root_off + 4, BS)   # size
    struct.pack_into("<H", itbl, root_off + 24, 2)   # links_count
    struct.pack_into("<I", itbl, root_off + 28, BS // 512)  # blocks (512-byte units)
    struct.pack_into("<I", itbl, root_off + 40, root_blk)   # block[0]
    # Mark root_blk used
    bm[root_blk // 8] |= 1 << (root_blk % 8)
    free_blocks -= 1

    # ---- root directory block ----
    dir_blk = bytearray(BS)
    # '.' entry: inode=2, rec_len=12, name_len=1, file_type=2
    struct.pack_into("<IHBBs", dir_blk, 0, EXT2_ROOT, 12, 1, 2, b'.')
    # '..' entry: inode=2, rec_len=BS-12, name_len=2, file_type=2
    struct.pack_into("<IHBBss", dir_blk, 12, EXT2_ROOT, BS - 12, 2, 2, b'.', b'.')

    # ---- superblock (written to block 1) ----
    # Layout matches struct ext2_superblock in kernel/include/ext2_fs.h:
    # off 0x00: inodes_count, blocks_count, reserved, free_blocks, free_inodes,
    #           first_data_block, log_block_size(0=1k), blocks_per_group, inodes_per_group,
    #           mtime, wtime, mnt_count(u16), max_mnt_count(u16),
    #           pad_magic(u16), state(u16), errors(u16), minor(u16),
    #           magic(u16), pad2(u16), first_inode(u32), inode_size(u16), ...
    sb = bytearray(BS)
    struct.pack_into("<IIIIIIIII", sb, 0x00,
        inodes_count, blocks_count, 0, free_blocks, free_inodes,
        first_data, 0,   # log_block_size = 0 means 1024
        8192, 2048)      # blocks_per_group, inodes_per_group
    struct.pack_into("<IIHHHHHH", sb, 0x24,
        0, 0,            # mtime, wtime
        0, 0xFFFF,       # mnt_count, max_mnt_count
        0, 1, 0, 0)      # pad_magic, state, errors, minor
    struct.pack_into("<HH", sb, 0x38, EXT2_MAGIC, 0)
    struct.pack_into("<IH", sb, 0x3C, 11, INODE_SIZE)   # first_inode, inode_size

    with open(path, "r+b") as f:
        # Block 0: boot (leave zero)
        # Block 1: superblock
        f.seek(BS)
        f.write(bytes(sb))
        # Block 2: block bitmap
        f.seek(2 * BS)
        f.write(bytes(bm))
        # Blocks 2..2+bb_blocks-1: block bitmap
        f.seek(2 * BS)
        f.write(bytes(bm))
        # Block ib_block: inode bitmap (padded to BS)
        f.seek(ib_block * BS)
        f.write(bytes(ibm).ljust(BS, b'\0'))
        # Blocks it_start..it_start+inode_tbl_blks-1: inode table
        f.seek(it_start * BS)
        f.write(bytes(itbl))
        # Root directory block
        f.seek(root_blk * BS)
        f.write(bytes(dir_blk))

    # Seed the standard directory skeleton so apps have somewhere to write.
    # /home/user is the default working directory for the GUI apps (the file
    # dialog, text editor, …); without it, saves into /home/user fail because
    # ext2_create does not create missing parent directories. /tmp backs the
    # file-dialog result hand-off. Reuse ext2_put's writer to avoid duplicating
    # the directory-allocation logic here.
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "ext2_put", os.path.join(os.path.dirname(os.path.abspath(__file__)), "ext2_put.py"))
    ext2_put = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ext2_put)

    fs = ext2_put.Ext2(path)
    def ensure(parent_ino, name):
        ino = fs.dir_find(parent_ino, name)
        return ino if ino else fs.mkdir(parent_ino, name)
    home = ensure(EXT2_ROOT, "home")
    ensure(home, "user")
    ensure(EXT2_ROOT, "tmp")
    fs.flush_meta()

    print(f"format_disk: formatted {path} ({disk_size//1024//1024} MB, "
          f"{blocks_count} blocks, {inodes_count} inodes, first_data={first_data}); "
          f"seeded /home/user, /tmp")

if __name__ == "__main__":
    main()
