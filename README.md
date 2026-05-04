# Triumph OS

A from-scratch operating system written in C, running on a Linux kernel with a
custom userspace. TTY-only, single static binary for the entire shell + apps.

Note: It is a stand-alone Linux distro as it is based off of the Linux kernel.

```
████████╗██████╗ ██╗██╗   ██╗███╗   ███╗██████╗ ██╗  ██╗
╚══██╔══╝██╔══██╗██║██║   ██║████╗ ████║██╔══██╗██║  ██║
   ██║   ██████╔╝██║██║   ██║██╔████╔██║██████╔╝███████║
   ██║   ██╔══██╗██║██║   ██║██║╚██╔╝██║██╔═══╝ ██╔══██║
   ██║   ██║  ██║██║╚██████╔╝██║ ╚═╝ ██║██║     ██║  ██║
   ╚═╝   ╚═╝  ╚═╝╚═╝ ╚═════╝ ╚═╝     ╚═╝╚═╝     ╚═╝  ╚═╝
```

## Features

- **Fullscreen menu launcher** — three-column TUI (help left, menu center,
  fetch info right). Boots directly into the menu.
- **Custom shell** with builtins, history, theming.
- **Games**: Snake, Tetris, Pongy, Chicken (Chrome-dino-style runner).
- **Calculator** — interactive UI with full expression parser.
- **Text editor** — nano-style.
- **File explorer** — arrow nav, Shift+R/E/D for read/edit/delete.
- **Web browser** — HTTP and HTTPS via mbedTLS, hand-rolled DHCP client.
- **Bundled Realtek r8169 driver + firmware** for ethernet on most laptops.
- **Boot melody**

## What's NOT here, on purpose

- No WiFi (would need wpa_supplicant + 100MB of firmware blobs) also helps privacy wise.
- No X11 / XFCE (would need ~1.5GB of dependencies), reduces bloat.
- No JavaScript / CSS rendering in the browser, helps privacy wise and reduces bloate.

## Building

Requires a Linux host with `gcc`, `make`, `libmbedtls-dev`, `grub-pc-bin`,
`xorriso`, `fakeroot`, `cpio`, and a kernel image with matching modules.

```sh
make iso
```

Or the manual steps:

```sh
gcc -O2 -static -w -o triumph triumph.c -lmbedtls -lmbedx509 -lmbedcrypto -lm
gcc -O2 -static -w -o init init.c
# ... set up initramfs, bundle r8169.ko + firmware + ca-certificates.crt
fakeroot bash -c 'cd initramfs && find . | cpio -o -H newc | gzip -9 > ../initramfs.img'
grub-mkrescue --output=triumph-os.iso iso/ --compress=xz
```
***Or just downloads the ISO from the 'Releases' menu.***

## Running

### QEMU
```sh
qemu-system-x86_64 -cdrom triumph-os.iso -m 512M
```

### USB on real hardware
OpenBSD:
```sh
doas dd if=triumph-os.iso of=/dev/rsd1c bs=1m
doas sync
```
Linux:
```sh
sudo dd if=triumph-os.iso of=/dev/sdX bs=4M status=progress && sync
```
macOS:
```sh
sudo dd if=triumph-os.iso of=/dev/rdiskN bs=4m
```

## Source layout

| File         | What it is                                              |
|--------------|---------------------------------------------------------|
| `init.c`     | PID 1 — mounts /proc /sys /dev, loads r8169 driver      |
| `triumph.c`  | The shell — readline, prompt, builtins, app dispatcher  |
| `menu.c`     | Fullscreen menu launcher                                |
| `editor.c`   | Nano-style text editor                                  |
| `snake.c`    | Snake game                                              |
| `tetris.c`   | Tetris game                                             |
| `pongy.c`    | Pong with AI opponent                                   |
| `chicken.c`  | Endless runner with a chicken                           |
| `calc_ui.c`  | Interactive calculator                                  |
| `files.c`    | File explorer                                           |
| `web.c`      | HTTP/HTTPS browser + DHCP client                        |
| `tools.c`    | Calculator backend, figlet, misc                        |
| `splash.c`   | Boot splash                                             |
| `beep.c`     | PC speaker sound effects                                |

## License

Public domain. Do whatever you want with it.
