#!/usr/bin/env bash
set -euo pipefail

echo "==> Installing dependencies..."
sudo apt-get update -qq
sudo apt-get install -y gcc grub-pc-bin grub-efi-amd64-bin \
  xorriso mtools dosfstools fakeroot linux-image-generic

KERNEL=$(ls /boot/vmlinuz-* | tail -1)
echo "==> Using kernel: $KERNEL"

echo "==> Compiling..."
cd src
gcc -O2 -static -w -o triumph triumph.c -lm
gcc -O2 -static -w -o init    init.c
cd ..

echo "==> Building initramfs..."
rm -rf _initramfs
mkdir -p _initramfs/bin _initramfs/sbin _initramfs/dev \
         _initramfs/proc _initramfs/sys _initramfs/tmp \
         _initramfs/run _initramfs/root _initramfs/etc \
         _initramfs/lib _initramfs/lib64 _initramfs/mnt

cp src/triumph _initramfs/bin/triumph
cp src/init    _initramfs/init
chmod +x _initramfs/init _initramfs/bin/triumph
ln -sf triumph _initramfs/bin/sh
ln -sf triumph _initramfs/bin/bash

echo "triumph"   > _initramfs/etc/hostname
printf 'root:x:0:0:root:/root:/bin/triumph\n' > _initramfs/etc/passwd
printf 'root:x:0:root\n' > _initramfs/etc/group

fakeroot bash -c '
  cd _initramfs
  mknod -m 666 dev/null    c 1 3
  mknod -m 666 dev/zero    c 1 5
  mknod -m 666 dev/urandom c 1 9
  mknod -m 600 dev/console c 5 1
  mknod -m 666 dev/tty     c 5 0
  mknod -m 620 dev/tty0    c 4 0
  mknod -m 620 dev/tty1    c 4 1
  mknod -m 660 dev/ttyS0   c 4 64
  find . | cpio -o -H newc 2>/dev/null | gzip -9 > ../initramfs.img
'

echo "==> Building ISO tree..."
mkdir -p _iso/boot/grub
cp "$KERNEL"    _iso/boot/vmlinuz
cp initramfs.img _iso/boot/initramfs.img

cat > _iso/boot/grub/grub.cfg << 'GRUBEOF'
set default=0
set timeout=5

menuentry "Triumph OS 1.0 (recommended)" --class gnu-linux {
    insmod gzio
    insmod part_msdos
    insmod part_gpt
    insmod ext2
    linux   /boot/vmlinuz root=/dev/ram0 rdinit=/init console=tty0 console=ttyS0,115200n8 nomodeset quiet loglevel=0
    initrd  /boot/initramfs.img
}

menuentry "Triumph OS 1.0 (verbose)" --class gnu-linux {
    insmod gzio
    insmod part_msdos
    insmod part_gpt
    insmod ext2
    linux   /boot/vmlinuz root=/dev/ram0 rdinit=/init console=tty0 console=ttyS0,115200n8 nomodeset
    initrd  /boot/initramfs.img
}

menuentry "Triumph OS 1.0 (serial console)" --class gnu-linux {
    insmod gzio
    linux   /boot/vmlinuz root=/dev/ram0 rdinit=/init console=ttyS0,115200n8 quiet loglevel=0
    initrd  /boot/initramfs.img
}
GRUBEOF

echo "==> Running grub-mkrescue..."
grub-mkrescue --output=triumph-os.iso _iso/ --compress=xz

echo ""
echo "Done! triumph-os.iso is ready."
echo "Flash: sudo dd if=triumph-os.iso of=/dev/sdX bs=4M status=progress oflag=sync"
echo "QEMU:  qemu-system-x86_64 -cdrom triumph-os.iso -m 512M -enable-kvm"
