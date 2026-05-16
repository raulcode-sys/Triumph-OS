#!/usr/bin/env python3
"""
gen_wallpaper.py  <input.png>  <output.h>
Converts a PNG to a C header with pixel data for Triumph OS framebuffer.
"""
import sys
from PIL import Image

def main():
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} input.png output.h")
        sys.exit(1)

    src, dst = sys.argv[1], sys.argv[2]
    img = Image.open(src).convert("RGB")
    img = img.resize((1920, 1080), Image.LANCZOS)
    w, h = img.size
    pixels = img.load()

    with open(dst, "w") as f:
        f.write("#pragma once\n")
        f.write(f"#define WP_W {w}\n")
        f.write(f"#define WP_H {h}\n")
        f.write("/* RGB stored as 0x00RRGGBB */\n")
        f.write("static const unsigned int wp_data[] = {\n")

        buf = []
        for y in range(h):
            for x in range(w):
                r, g, b = pixels[x, y]
                buf.append(f"0x{r:02x}{g:02x}{b:02x}")
            if len(buf) >= 1920 * 10:
                f.write(",".join(buf) + ",\n")
                buf = []
                sys.stderr.write(f"\r{y+1}/{h} rows")

        if buf:
            f.write(",".join(buf) + "\n")
        f.write("};\n")

    sys.stderr.write(f"\nDone → {dst}\n")

if __name__ == "__main__":
    main()
