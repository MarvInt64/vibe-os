#!/bin/bash
# Creates an ext2 disk image with VibeOS binaries

set -e

DISK_SIZE_MB=10
DISK_IMG="build/disk.img"

echo "Creating disk image (${DISK_SIZE_MB}MB)..."

# Create empty disk image
dd if=/dev/zero of="$DISK_IMG" bs=1M count=$DISK_SIZE_MB status=progress

# Create ext2 filesystem using Docker
echo "Creating ext2 filesystem..."
docker run --rm -v "$(pwd):/data" -w /data alpine:latest \
    sh -c "apk add --no-cache e2fsprogs && mkfs.ext2 -F -b 4096 /data/build/disk.img"

# Mount and copy files using Docker
echo "Mounting disk image..."
docker run --rm --privileged -v "$(pwd):/data" alpine:latest \
    sh -c "
        apk add --no-cache e2fsprogs
        
        # Mount the disk image
        mkdir -p /mnt/disk
        mount /data/build/disk.img /mnt/disk
        
        echo 'Creating directory structure...'
        mkdir -p /mnt/disk/bin
        mkdir -p /mnt/disk/etc
        mkdir -p /mnt/disk/home
        
        echo 'Copying binaries...'
        ls -la /data/build/user/
        cp /data/build/user/sh.elf /mnt/disk/bin/sh
        chmod +x /mnt/disk/bin/sh
        echo 'Shell copied successfully'
        
        echo 'Creating motd...'
        echo 'Welcome to VibeOS!' > /mnt/disk/etc/motd
        
        echo 'Verifying disk contents:'
        ls -la /mnt/disk/bin/
        ls -la /mnt/disk/etc/
        
        # Flush and unmount
        sync
        umount /mnt/disk
        echo 'Unmounted successfully'
    "

echo ""
echo "Disk image created: $DISK_IMG"
ls -lh "$DISK_IMG"
echo ""
echo "Disk image ready to use with: qemu-system-x86_64 ... -hda $DISK_IMG"
