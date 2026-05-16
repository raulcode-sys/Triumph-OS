CC      = gcc
CFLAGS  = -O2 -static -w
LDFLAGS = -lm -lmbedtls -lmbedx509 -lmbedcrypto

all: triumph init

triumph: triumph.c editor.c snake.c tetris.c tools.c fb.c wallpaper.h
	$(CC) $(CFLAGS) -o triumph triumph.c $(LDFLAGS)

init: init.c splash.c audio.c
	$(CC) $(CFLAGS) -o init init.c

clean:
	rm -f triumph init

# ── ISO build ──────────────────────────────────────────────────────────────
# Requires: grub-pc-bin xorriso fakeroot cpio gzip
# Directory layout expected:
#   initramfs/   ← populated below
#   iso/boot/grub/grub.cfg
#   iso/boot/vmlinuz   ← copy your kernel here
iso: triumph init
	mkdir -p initramfs/bin initramfs/dev initramfs/proc initramfs/sys initramfs/tmp initramfs/run initramfs/root initramfs/lib initramfs/persist
	cp triumph  initramfs/bin/triumph
	cp init     initramfs/init
	chmod +x    initramfs/init initramfs/bin/triumph
	ln -sf triumph initramfs/bin/sh
	# copy kernel modules if present
	-cp -r /lib/modules initramfs/lib/ 2>/dev/null
	-cp -r /lib/firmware initramfs/lib/ 2>/dev/null
	# pack initramfs
	fakeroot bash -c 'cd initramfs && find . | cpio -o -H newc | gzip -9 > ../initramfs.img'
	mkdir -p iso/boot/grub
	cp initramfs.img iso/boot/initramfs.img
	# grub.cfg
	echo 'set timeout=3'                                                                         > iso/boot/grub/grub.cfg
	echo 'set default=0'                                                                        >> iso/boot/grub/grub.cfg
	echo 'insmod all_video'                                                                     >> iso/boot/grub/grub.cfg
	echo 'insmod video_fb'                                                                      >> iso/boot/grub/grub.cfg
	echo 'insmod gfxterm'                                                                       >> iso/boot/grub/grub.cfg
	echo 'set gfxmode=1920x1080x32,1280x720x32,auto'                                           >> iso/boot/grub/grub.cfg
	echo 'terminal_output gfxterm'                                                              >> iso/boot/grub/grub.cfg
	echo 'menuentry "Triumph OS" {'                                                             >> iso/boot/grub/grub.cfg
	echo '  linux  /boot/vmlinuz quiet loglevel=0 fbcon=map:0 video=efifb:on,1920x1080-32'    >> iso/boot/grub/grub.cfg
	echo '  initrd /boot/initramfs.img'                                                        >> iso/boot/grub/grub.cfg
	echo '}'                                                                                    >> iso/boot/grub/grub.cfg
	echo 'menuentry "Triumph OS (fallback 720p)" {'                                            >> iso/boot/grub/grub.cfg
	echo '  linux  /boot/vmlinuz quiet loglevel=0 fbcon=map:0 video=efifb:on,1280x720-32'     >> iso/boot/grub/grub.cfg
	echo '  initrd /boot/initramfs.img'                                                        >> iso/boot/grub/grub.cfg
	echo '}'                                                                                    >> iso/boot/grub/grub.cfg
	echo 'menuentry "Triumph OS (VESA legacy)" {'                                              >> iso/boot/grub/grub.cfg
	echo '  linux  /boot/vmlinuz quiet loglevel=0 video=vesafb:1920x1080-32'                  >> iso/boot/grub/grub.cfg
	echo '  initrd /boot/initramfs.img'                                                        >> iso/boot/grub/grub.cfg
	echo '}'                                                                                    >> iso/boot/grub/grub.cfg
	grub-mkrescue --output=triumph-os.iso iso/ --compress=xz
	@echo ""
	@echo ">>> triumph-os.iso built successfully <<<"
