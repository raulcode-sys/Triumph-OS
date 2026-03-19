# Triumph OS

A minimal TTY-only live OS built from scratch in C. Boot from USB, no installation required.

## What it is

- **triumph** — a bash-like shell written in C (~800 lines). Runs as the login shell.
- **init** — a tiny PID 1 init process in C. Mounts `/proc`, `/sys`, `/dev`, sets up the TTY, then spawns the shell.
- **editor** — a nano-like text editor built directly into the shell (`nano` / `edit` / `vi`).
- Linux kernel + GRUB as the bootloader. That's the entire OS.

## Building the ISO

You need Debian/Ubuntu with these packages:
```
sudo apt install gcc grub-pc-bin grub-efi-amd64-bin xorriso mtools fakeroot linux-image-generic
```

Then run the build script:
```bash
chmod +x build.sh
./build.sh
```

Output: `triumph-os.iso`

## Flashing to USB

```bash
sudo dd if=triumph-os.iso of=/dev/sdX bs=4M status=progress oflag=sync
```

Or use Rufus on Windows (DD mode).

## Running in QEMU

```bash
qemu-system-x86_64 -cdrom triumph-os.iso -m 512M
# faster:
qemu-system-x86_64 -cdrom triumph-os.iso -m 512M -enable-kvm
```

## Shell features

- Coloured two-line prompt with `╭─` style
- Arrow key history navigation
- Tab completion (builtins + files)
- Pipes `|`, redirection `> >> < 2>`, background `&`
- Variable expansion `$VAR $? ${VAR}`
- Glob expansion `*.c file?.txt`
- `&&` and `||` operators
- Built-in commands: `ls cat grep head tail wc find du df ps kill stat file mkdir rm mv cp touch chmod ln sleep date uname hostname whoami id uptime free history export alias source test read echo printf clear fetch help exit`
- Built-in nano-like editor: `nano file.txt` / `edit file.txt`
- Built-in `fetch` (neofetch-style system info panel)
- Default aliases: `ll` `la` `..` `...` `cls`

## Editor keybindings

| Key | Action |
|-----|--------|
| `Ctrl-S` | Save |
| `Ctrl-X` | Exit (prompts if unsaved) |
| `Ctrl-K` | Cut line |
| `Ctrl-U` | Paste line |
| `Ctrl-W` | Search |
| `Ctrl-N` | Find next |
| `Ctrl-A` / `Home` | Start of line |
| `Ctrl-E` / `End` | End of line |
| Arrow keys | Move cursor |
| `Tab` | Insert 4 spaces |

## Project structure

```
src/
  triumph.c   — shell (includes editor.c)
  editor.c    — text editor (included by triumph.c)
  init.c      — PID 1 init
  Makefile
build.sh      — builds the ISO from scratch
README.md
```

## License

Public domain. Do whatever you want with it.
