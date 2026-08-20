#!/usr/bin/env python3
"""Bake data/images/no-phone.png — the card the source shows when no phone is connected.

Baked rather than drawn at runtime on purpose: drawing this in C would mean shipping a
font rasteriser and a text layout engine into the plugin for one static card. The QR
modules come from Nayuki's qrcodegen (MIT) via tools/emit_qr, so the codes are real and
verifiable rather than traced artwork.

Regenerate after changing any copy:
    cc -O2 -o build-aux/placeholder/emit_qr build-aux/placeholder/emit_qr.c \
             build-aux/placeholder/qrcodegen.c
    python3 build-aux/placeholder/make-placeholder.py
"""
import subprocess, sys, pathlib
from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parents[2]
HERE = pathlib.Path(__file__).resolve().parent
EMIT = HERE / "emit_qr"
PREVIEW = HERE / "no-phone-preview.png"            # human-viewable design output
OUT = ROOT / "data" / "images" / "no-phone.bin"    # what actually ships

W, H = 1280, 720
BG = (18, 18, 22)
CARD = (28, 28, 34)
EDGE = (58, 58, 68)
FG = (245, 245, 247)
DIM = (150, 150, 160)
BODY = (216, 216, 222)
ACCENT = (124, 92, 214)

# ONE code, pointed at the download page rather than at the stores directly. Baking store
# URLs into a shipped binary asset would freeze them: adding a platform or changing a store
# link would need a new plugin release. The page already carries every badge, so the phone
# picks its own platform there, and this artwork never has to change for it.
DOWNLOAD_URL = "https://adewaskar.com/apps/spancam"


def qr_image(text, px):
    out = subprocess.run([str(EMIT), text], capture_output=True, text=True, check=True).stdout.split()
    n = int(out[0])
    grid = out[1:1 + n]
    quiet = 3
    side = n + quiet * 2
    img = Image.new("L", (side, side), 255)
    for y in range(n):
        for x in range(n):
            if grid[y][x] == "1":
                img.putpixel((x + quiet, y + quiet), 0)
    # NEAREST keeps module edges hard, which is what a scanner wants
    return img.resize((px, px), Image.NEAREST).convert("RGB")


def font(sz, bold=False):
    for p in ("/System/Library/Fonts/Supplemental/Helvetica.ttc",
              "/System/Library/Fonts/HelveticaNeue.ttc",
              "/System/Library/Fonts/Supplemental/Arial.ttf"):
        try:
            return ImageFont.truetype(p, sz, index=1 if bold else 0)
        except Exception:
            pass
    return ImageFont.load_default()


def main():
    if not EMIT.exists():
        sys.exit(f"missing {EMIT} — build it first (see docstring)")
    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)

    x0, y0, x1, y1 = 90, 96, W - 90, H - 96
    d.rounded_rectangle([x0, y0, x1, y1], radius=22, fill=CARD, outline=EDGE, width=2)

    # one large code — this is read off a monitor, often from a metre away
    qpx = 300
    qx, qy = x0 + 52, y0 + 88
    d.rounded_rectangle([qx - 14, qy - 14, qx + qpx + 14, qy + qpx + 14], radius=12,
                        fill=(255, 255, 255))
    img.paste(qr_image(DOWNLOAD_URL, qpx), (qx, qy))

    tx = qx + qpx + 78
    d.text((tx, y0 + 76), "No phone connected", font=font(46, True), fill=FG)
    d.text((tx, y0 + 140), "Scan to install Spancam on your phone.", font=font(24), fill=DIM)

    ty = y0 + 208
    for n, line in enumerate((
        "Scan the code and install Spancam.",
        "Open it and turn on Discoverable.",
        "Join the same Wi-Fi, or plug in USB.",
        "In OBS, click Refresh and pick the phone.",
    ), 1):
        d.ellipse([tx, ty + 2, tx + 30, ty + 32], fill=ACCENT)
        nw = d.textlength(str(n), font=font(17, True))
        d.text((tx + (30 - nw) / 2, ty + 8), str(n), font=font(17, True), fill=(255, 255, 255))
        d.text((tx + 46, ty + 5), line, font=font(23), fill=BODY)
        ty += 56

    d.text((tx, y1 - 74), "adewaskar.com/apps/spancam", font=font(21), fill=(120, 120, 132))

    PREVIEW.parent.mkdir(parents=True, exist_ok=True)
    img.save(PREVIEW, optimize=True)
    print(f"wrote {PREVIEW.relative_to(ROOT)}  {W}x{H}  {PREVIEW.stat().st_size} bytes (preview)")

    # Ship a run-length-encoded BGRA blob, not the PNG. A PNG would mean vendoring a
    # decoder (~250 KB of third-party source) into a plugin that currently has no
    # dependencies at all, to read one file we produce ourselves. RLE over 32-bit pixels
    # costs ~166 KB on disk instead of 39 KB, and about forty lines of C to expand.
    px = img.convert("RGBA").tobytes()
    runs = []
    i, total = 0, W * H
    while i < total:
        r, g, b, a = px[i * 4:i * 4 + 4]
        bgra = b | (g << 8) | (r << 16) | (a << 24)
        n = 1
        while i + n < total and n < 0xFFFF and px[(i + n) * 4:(i + n) * 4 + 4] == px[i * 4:i * 4 + 4]:
            n += 1
        runs.append((n, bgra))
        i += n

    import struct
    blob = bytearray(struct.pack("<4sIIII", b"SPCP", 1, W, H, len(runs)))
    for n, bgra in runs:
        blob += struct.pack("<HI", n, bgra)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(blob)
    assert sum(n for n, _ in runs) == total, "run lengths do not cover the image"
    print(f"wrote {OUT.relative_to(ROOT)}  {len(runs)} runs  {OUT.stat().st_size} bytes (shipped)")


if __name__ == "__main__":
    main()
