# TTF -> Adafruit GFX font header, 1 bit (no anti-aliasing), optional slant
# usage: ttf2gfx.py font.ttf size name [slant] [license line] > name.h
import sys
from PIL import Image, ImageDraw, ImageFont
path, size, name = sys.argv[1], int(sys.argv[2]), sys.argv[3]
slant = float(sys.argv[4]) if len(sys.argv) > 4 else 0.0
license = sys.argv[5] if len(sys.argv) > 5 else ""
f = ImageFont.truetype(path, size)
ascent, descent = f.getmetrics()
CW, CH, BX, BY = size * 4, size * 3, size, size * 2   # canvas, baseline origin
bits, glyphs = [], []
for c in range(0x20, 0x7F):
    ch = chr(c)
    img = Image.new("L", (CW, CH), 0); d = ImageDraw.Draw(img); d.fontmode = "1"
    d.text((BX, BY), ch, font=f, fill=255, anchor="ls")
    if slant:  # x' = x + (baseline - y) * slant
        img = img.transform(img.size, Image.AFFINE, (1, slant, -slant * BY, 0, 1, 0), resample=Image.NEAREST)
    img = img.point(lambda v: 255 if v > 127 else 0)
    box = img.getbbox()
    adv = int(round(f.getlength(ch)))
    if not box:
        glyphs.append((len(bits) // 8, 0, 0, adv, 0, 0)); continue
    l, t, r, b = box
    for y in range(t, b):
        for x in range(l, r):
            bits.append(1 if img.getpixel((x, y)) else 0)
    while len(bits) % 8: bits.append(0)
    glyphs.append(((len(bits) - ((r - l) * (b - t) + 7) // 8 * 8) // 8, r - l, b - t, adv, l - BX, t - BY))
data = [int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8)]
src = path.split("/")[-1]
print("// %s %d px%s, converted from %s with software/lib/nemesis/src/fonts/ttf2gfx.py" % (name, size, " oblique (slant %.2f)" % slant if slant else "", src))
if license:
    print("// " + license)
print("#pragma once\n#include <Adafruit_GFX.h>\n")
print("const uint8_t %sBitmaps[] PROGMEM = {" % name)
for i in range(0, len(data), 16):
    print("  " + ", ".join("0x%02X" % v for v in data[i:i + 16]) + ",")
print("};\n\nconst GFXglyph %sGlyphs[] PROGMEM = {" % name)
for c, g in zip(range(0x20, 0x7F), glyphs):
    print("  {%5d, %3d, %3d, %3d, %4d, %4d}, // 0x%02X %s" % (g + (c, chr(c) if c != 0x5C else "backslash")))
print("};\n\nconst GFXfont %s PROGMEM = {(uint8_t *)%sBitmaps, (GFXglyph *)%sGlyphs, 0x20, 0x7E, %d};" % (name, name, name, ascent + descent))
