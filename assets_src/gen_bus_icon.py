#!/usr/bin/env python3
"""
Generates bus_icons.h -- a small, self-contained addition to this project's
icon set (kept SEPARATE from icons.h rather than merged into it, since the
original icons.h generator script no longer exists anywhere on this machine
-- see CLAUDE.md's Asset Generation Pipeline note about that. Re-deriving/
guessing the packing convention would risk producing bytes incompatible
with the real one; instead this was reverse-engineered by reading
renderer.cpp's actual consumer code (drawPortraitBitmap()/
portraitFillRect()) directly, not assumed:

  - Row-padded 4bpp: each row occupies ceil(width/2) bytes independently
    (confirmed via portraitFillRect()'s `rowBytes = (width+1)/2` and by
    checking existing icons.h sizes against width*height/2 by hand for
    several icons of different dimensions -- all consistent with row
    padding, e.g. icon_sun 130x130 -> 65 bytes/row * 130 rows = 8450,
    matching icon_sun_data[8450] exactly).
  - nibble = pixel_8bit >> 4 (0=black .. 15=white).
  - Within a row: column 0 (even) -> low nibble of byte 0, column 1 (odd)
    -> high nibble of byte 0, column 2 -> low nibble of byte 1, etc. An
    odd-width row's final half-byte is padding (unused high nibble).
  - Every bitmap is pre-rotated 90 deg (PIL ROTATE_90) before packing, same
    as every other bitmap in this project -- portrait mode is achieved
    entirely by pre-rotating assets + a coordinate transform, not by any
    runtime rotation (see CLAUDE.md's "Portrait Mode" section).

Regenerate: `python3 gen_bus_icon.py > bus_icons.h` (or just edit this
script and re-run if the icon design needs to change).
"""
from PIL import Image, ImageDraw

SIZE = 34  # matches icon_wind/icon_humidity/icon_sunrise/icon_sunset/icon_location/icon_clock exactly


def draw_bus_glyph():
    """A simple, solid bus silhouette -- rounded body + two wheels + a
    couple of window cutouts -- in the same minimal, solid-black-on-white
    style as this project's other small inline icons (not photorealistic,
    just legible at ~34px)."""
    img = Image.new("L", (SIZE, SIZE), 255)  # 255 = white background
    d = ImageDraw.Draw(img)

    body_left, body_top = 3, 4
    body_right, body_bottom = 31, 24
    d.rounded_rectangle([body_left, body_top, body_right, body_bottom], radius=5, fill=0)

    # Window cutouts (punch back to white) -- three small windows across the top
    win_top = body_top + 4
    win_bottom = body_top + 11
    win_w = 6
    gap = 2
    x = body_left + 3
    for _ in range(3):
        d.rectangle([x, win_top, x + win_w, win_bottom], fill=255)
        x += win_w + gap

    # Wheels -- two black circles straddling the bottom edge of the body
    wheel_r = 4
    for cx in (body_left + 7, body_right - 7):
        cy = body_bottom
        d.ellipse([cx - wheel_r, cy - wheel_r, cx + wheel_r, cy + wheel_r], fill=0)

    return img


def pack_4bpp_rowpadded(img):
    """img: PIL 'L' image, ALREADY rotated (this is what actually gets
    packed/stored). Row-padded 4bpp, matching renderer.cpp's consumer
    exactly (see module docstring)."""
    w, h = img.size
    px = img.load()
    row_bytes = (w + 1) // 2
    out = bytearray(row_bytes * h)
    for y in range(h):
        for x in range(w):
            val = px[x, y] >> 4  # 8-bit gray -> 4-bit nibble, 0=black..15=white
            byte_index = y * row_bytes + (x // 2)
            if x % 2 == 0:
                out[byte_index] = (out[byte_index] & 0xF0) | val
            else:
                out[byte_index] = (out[byte_index] & 0x0F) | (val << 4)
    return bytes(out)


def emit(name, upright_img):
    owidth, oheight = upright_img.size
    rotated = upright_img.transpose(Image.ROTATE_90)
    rw, rh = rotated.size
    data = pack_4bpp_rowpadded(rotated)

    print(f"const uint32_t {name}_owidth = {owidth};")
    print(f"const uint32_t {name}_oheight = {oheight};")
    print(f"const uint32_t {name}_width = {rw};")
    print(f"const uint32_t {name}_height = {rh};")
    print(f"const uint8_t {name}_data[{len(data)}] = {{")
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        print("    " + ", ".join(f"0x{b:02X}" for b in chunk) + ",")
    print("};")
    print()


def main():
    print("#pragma once")
    print("#include <stdint.h>")
    print()
    print("// Auto-generated PORTRAIT-mode bitmap (4bpp, row-padded packing) for the")
    print("// bus section (see CLAUDE.md's \"Planned UI\"/\"Real-Time Bus Data\" and")
    print("// this file's own generator, gen_bus_icon.py, kept in proto_src/ alongside")
    print("// the other regeneration sources). Deliberately a SEPARATE file from")
    print("// icons.h -- that file's original generator script no longer exists on")
    print("// this machine (see icons.h's own header comment), so this was reverse-")
    print("// engineered from renderer.cpp's actual consumer code instead of guessed,")
    print("// and kept out of icons.h to avoid ever needing to touch/regenerate that")
    print("// file's existing, working, hand-verified assets.")
    print("// *_owidth/*_oheight: ORIGINAL (pre-rotation) dims -- use for portrait-space")
    print("// layout math. *_width/*_height: stored/rotated dims -- pass to the blit call.")
    print()
    emit("icon_bus", draw_bus_glyph())


if __name__ == "__main__":
    main()
