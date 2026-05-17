CC      = gcc
CFLAGS  = -O2 -static -w
LDFLAGS = -lm -lmbedtls -lmbedx509 -lmbedcrypto -lpthread -lutil

all: triumph init

triumph: triumph.c editor.c snake.c tetris.c tools.c fb.c wallpaper.h
	$(CC) $(CFLAGS) -o triumph triumph.c $(LDFLAGS)

init: init.c splash.c audio.c
	$(CC) $(CFLAGS) -o init init.c

clean:
	rm -f triumph init

iso: triumph init
	mkdir -p initramfs/bin initramfs/dev initramfs/proc initramfs/sys initramfs/tmp initramfs/run initramfs/root initramfs/lib initramfs/persist
	cp triumph  initramfs/bin/triumph
	cp init     initramfs/init
	chmod +x    initramfs/init initramfs/bin/triumph
	ln -sf triumph initramfs/bin/sh
	-cp -r /lib/modules initramfs/lib/ 2>/dev/null
	-cp -r /lib/firmware initramfs/lib/ 2>/dev/null
	fakeroot bash -c 'cd initramfs && find . | cpio -o -H newc | gzip -9 > ../initramfs.img'
	mkdir -p iso/boot/grub
	cp initramfs.img iso/boot/initramfs.img
	printf 'set timeout=5\n'                                                          > iso/boot/grub/grub.cfg
	printf 'set default=0\n'                                                         >> iso/boot/grub/grub.cfg
	printf 'terminal_input console\n'                                                >> iso/boot/grub/grub.cfg
	printf 'terminal_output console\n'                                               >> iso/boot/grub/grub.cfg
	printf 'menuentry "Triumph OS" {\n'                                              >> iso/boot/grub/grub.cfg
	printf '  linux  /boot/vmlinuz quiet loglevel=0 video=efifb:on\n'              >> iso/boot/grub/grub.cfg
	printf '  initrd /boot/initramfs.img\n'                                          >> iso/boot/grub/grub.cfg
	printf '}\n'                                                                     >> iso/boot/grub/grub.cfg
	printf 'menuentry "Triumph OS (720p)" {\n'                                       >> iso/boot/grub/grub.cfg
	printf '  linux  /boot/vmlinuz quiet loglevel=0 video=efifb:on,1280x720-32\n' >> iso/boot/grub/grub.cfg
	printf '  initrd /boot/initramfs.img\n'                                          >> iso/boot/grub/grub.cfg
	printf '}\n'                                                                     >> iso/boot/grub/grub.cfg
	printf 'menuentry "Triumph OS (VESA)" {\n'                                       >> iso/boot/grub/grub.cfg
	printf '  linux  /boot/vmlinuz quiet loglevel=0 video=vesafb\n'                >> iso/boot/grub/grub.cfg
	printf '  initrd /boot/initramfs.img\n'                                          >> iso/boot/grub/grub.cfg
	printf '}\n'                                                                     >> iso/boot/grub/grub.cfg
	grub-mkrescue --output=triumph-os.iso iso/ --compress=xz
	@echo ""
	@echo ">>> triumph-os.iso built successfully <<<"
