# Triumph OS

It is solely meant for experimenting, if you decide to use it for daily use, you do you.

A from-scratch operating system written in C, running on a Linux kernel with a custom userspace. Single static binary for the entire shell, compositor, and apps.
It is important to understand it uses the Linux kernel.

Triumph is a standalone Linux distro — not a desktop environment or a container. It boots from USB on real hardware, renders a wallpaper to the framebuffer, and gives you a keyboard-driven shell compositor with games, a web browser, a file explorer, and more.


## What's new in v2

- **Framebuffer wallpaper** — boots to a fullscreen wallpaper rendered directly to `/dev/fb0`. No X11, no Wayland, no display server. Just raw pixels on the framebuffer.
- **Shell compositor** — keyboard-driven session switching between wallpaper, terminal, and menu. Think riced Hyprland but on bare metal with zero dependencies.
- **Shift+T** opens a fullscreen terminal over the wallpaper. Shift+T again closes it and brings back the wallpaper.
- **Shift+M** opens the menu launcher over the wallpaper. ESC closes it.
- **External keyboard support** — USB and Bluetooth keyboards are detected via `/dev/input/event*` and Shift+M / Shift+T work from them too.
- **Multi-GPU support** — works with Intel, AMD, and Nvidia GPUs. Reads actual RGB channel offsets from the framebuffer driver so it handles XRGB, BGRX, and other pixel layouts automatically.
- **GRUB boot menu** with three options: efifb (default), 720p fallback, and VESA legacy for older hardware.
- **CI/CD** — ISO builds automatically via GitHub Actions on every push. Tagged releases get the ISO attached to the GitHub Release.

## Features

- **Framebuffer wallpaper** — fullscreen image rendered to `/dev/fb0` with a background keeper thread. Supports any resolution, auto-scaled from 1920×1080 source.
- **Shell compositor** — Shift+T for terminal, Shift+M for menu, wallpaper on idle. No window stacking needed — fullscreen apps only, like dwm or ratpoison.
- **Fullscreen menu launcher** — three-column TUI with help, menu, and system fetch info. Navigate with arrow keys, launch apps with Enter.
- **Custom shell** — readline with tab completion, history, aliases, pipes, redirects, job control, environment variables, and theming.
- **Games** — Snake, Tetris, Pongy (Pong with AI), Chicken (Chrome dino-style runner). All with PC speaker sound effects and WAV audio.
- **Calculator** — interactive fullscreen UI with a recursive descent expression parser. Supports `+`, `-`, `*`, `/`, `%`, `^`, and parentheses.
- **Text editor** — nano-style with syntax-free editing.
- **File explorer** — arrow key navigation, read, edit, delete files. Shows file sizes and directory structure.
- **Web browser** — HTTP and HTTPS via mbedTLS, with a hand-rolled DHCP client. Text-only rendering, no JavaScript or CSS.
- **Bundled Realtek r8169 driver + firmware** — ethernet works out of the box on most laptops.
- **HDA audio support** — loads Intel HDA and Realtek codec modules for WAV playback.
- **Boot melody and game sound effects** via PC speaker.
- **Persistent storage** — optional ext4 partition on USB for saving accounts and files across reboots. Run `setup-persist` from the shell.

## What's NOT here, on purpose

- **No WiFi** — would need wpa_supplicant + 100MB+ of firmware blobs. Ethernet only. Also better for privacy.
- **No X11 / Wayland** — would need ~1.5GB of dependencies. The framebuffer compositor handles everything.
- **No JavaScript / CSS in the browser** — text-only rendering. Faster, lighter, more private.
- **No login screen** — boots straight to the wallpaper. Run `setup-persist` if you want user accounts.

## Building

### From GitHub Actions (easiest)

Download the ISO from the [Releases](../../releases) page. Every tagged release builds automatically.

### From source

Requires a Linux host (or GitHub Actions) with:

```
gcc make libmbedtls-dev grub-pc-bin grub-common xorriso fakeroot cpio gzip mtools
```

```sh
make iso
```

### On macOS

Use the GitHub Actions CI, or run in Docker:

```sh
docker run -it --rm -v $(pwd):/src ubuntu:24.04 bash
apt update && apt install -y gcc make libmbedtls-dev grub-pc-bin xorriso fakeroot cpio gzip mtools
cd /src && make iso
```

### Swapping the wallpaper

```sh
python3 gen_wallpaper.py your_image.png wallpaper.h
make iso
```

## Running

### USB on real hardware (recommended)

macOS:
```sh
diskutil list                          # find USB, e.g. /dev/disk2
diskutil unmountDisk /dev/disk2
sudo dd if=triumph-os.iso of=/dev/rdisk2 bs=4m
sudo sync
```

Linux:
```sh
sudo dd if=triumph-os.iso of=/dev/sdX bs=4M status=progress && sync
```

OpenBSD:
```sh
doas dd if=triumph-os.iso of=/dev/rsd1c bs=1m
doas sync
```

Or use software like Balena Etcher.

Boot from USB — press your boot key at startup for the boot menu. Select **Triumph OS** from GRUB.

### QEMU

```sh
qemu-system-x86_64 -cdrom triumph-os.iso -m 512M -vga std
```

For UEFI mode (closer to real hardware):
```sh
qemu-system-x86_64 \
  -cdrom triumph-os.iso \
  -m 512M \
  -vga std \
  -cpu max \
  -drive if=pflash,format=raw,readonly=on,file=/path/to/edk2-x86_64-code.fd
```

## Keybinds

| Key | Action |
|-----|--------|
| **Shift+T** | Open / close terminal |
| **Shift+M** | Open menu |
| **ESC** | Close menu |
| **↑ ↓** | Navigate menu |
| **Enter** | Launch selected app |

### In games

| Game | Controls |
|------|----------|
| Snake | WASD move, Ctrl+X quit |
| Tetris | ←→ move, ↑ rotate, Space drop, Ctrl+X quit |
| Pongy | W/S move, Ctrl+P pause, Ctrl+X quit |
| Chicken | Space jump, R retry, Ctrl+X quit |

## Source layout

| File | What it is |
|------|------------|
| `init.c` | PID 1 — mounts filesystems, loads drivers, plays boot melody |
| `triumph.c` | Shell — readline, prompt, builtins, app dispatcher |
| `fb.c` | Framebuffer compositor — wallpaper, session switching, external keyboard |
| `wallpaper.h` | Wallpaper image as a C array (1920×1080 XRGB) |
| `menu.c` | Fullscreen three-column menu launcher |
| `editor.c` | Nano-style text editor |
| `snake.c` | Snake game |
| `tetris.c` | Tetris game |
| `pongy.c` | Pong with AI opponent |
| `chicken.c` | Endless runner |
| `calc_ui.c` | Interactive calculator UI |
| `files.c` | File explorer |
| `web.c` | HTTP/HTTPS browser + DHCP client |
| `tools.c` | Calculator backend, figlet, misc utilities |
| `splash.c` | Boot splash animation |

| `setup_persist.c` | Persistent USB storage setup |
| `gen_wallpaper.py` | Converts PNG → wallpaper.h |
