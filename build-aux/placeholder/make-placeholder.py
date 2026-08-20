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
# Two layouts, because one does not survive both orientations: a landscape card centred
# in a 1080x1920 canvas leaves most of the width empty and the text ends up small. The
# plugin picks by canvas aspect at runtime.
LAYOUTS = ("landscape", "portrait")

# The CARD only — no surrounding canvas. The plugin composes this onto a
# background sized to the OBS canvas, so one asset serves any resolution or
# orientation and the text never gets letterboxed down to illegibility.
W, H = 1040, 470
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


STEPS = (
    "Scan the code and install Spancam.",
    "Open it and turn on Discoverable.",
    "Join the same Wi-Fi, or plug in USB.",
    "In OBS, click Refresh and pick the phone.",
)
MARK = HERE / "spancam-mark.png"   # extracted from cmake/macos/resources/spancam.icns
BRAND = "Spancam for OBS"
TITLE = "No phone connected"
SUB = "Scan to install Spancam on your phone."
FOOTER = "adewaskar.com/apps/spancam"


def card(orientation):
    """Draw the card for one orientation and return the image."""
    pad = 44
    if orientation == "landscape":
        W, H, qpx = 1040, 500, 300
    else:
        W, H, qpx = 760, 900, 340

    img = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([1, 1, W - 2, H - 2], radius=22, fill=CARD, outline=EDGE, width=2)

    def brand(x, y, centred):
        """Icon + wordmark. Rounded like the app icon is everywhere else."""
        size = 34
        mark = Image.open(MARK).convert("RGBA").resize((size, size), Image.LANCZOS)
        r = int(size * 0.26)
        mask = Image.new("L", (size, size), 0)
        ImageDraw.Draw(mask).rounded_rectangle([0, 0, size - 1, size - 1], radius=r, fill=255)
        fnt = font(21, True)
        tw = d.textlength(BRAND, font=fnt)
        total = size + 12 + tw
        bx = (W - total) / 2 if centred else x
        img.paste(mark, (int(bx), y), mask)
        d.text((bx + size + 12, y + 6), BRAND, font=fnt, fill=FG)
        return size

    def qr_block(x, y):
        d.rounded_rectangle([x - 12, y - 12, x + qpx + 12, y + qpx + 12], radius=12,
                            fill=(255, 255, 255))
        img.paste(qr_image(DOWNLOAD_URL, qpx), (x, y))

    if orientation == "landscape":
        # code on the left, everything else in a column beside it
        qr_block(pad, (H - qpx) // 2)
        tx = pad + qpx + 62
        centre = False
        brand(tx, pad, False)
        ty = pad + 58
    else:
        # code on top, everything else stacked underneath and centred
        brand(0, pad, True)
        qr_block((W - qpx) // 2, pad + 62)
        tx = pad
        centre = True
        ty = pad + 62 + qpx + 44

    def line(text, fnt, fill, y):
        w = d.textlength(text, font=fnt)
        d.text(((W - w) / 2 if centre else tx, y), text, font=fnt, fill=fill)

    line(TITLE, font(44 if orientation == "landscape" else 40, True), FG, ty)
    line(SUB, font(23 if orientation == "landscape" else 22), DIM, ty + 62)

    sy = ty + 116
    # In portrait the steps stay left-aligned as a block, but the block itself is
    # centred — a centred ragged list of four sentences is much harder to read.
    step_w = max(d.textlength(t, font=font(22)) for t in STEPS) + 44
    sx = (W - step_w) / 2 if centre else tx
    for n, text in enumerate(STEPS, 1):
        d.ellipse([sx, sy + 2, sx + 28, sy + 30], fill=ACCENT)
        nw = d.textlength(str(n), font=font(16, True))
        d.text((sx + (28 - nw) / 2, sy + 7), str(n), font=font(16, True), fill=(255, 255, 255))
        d.text((sx + 44, sy + 4), text, font=font(22), fill=BODY)
        sy += 52

    line(FOOTER, font(20), (120, 120, 132), H - pad - 22)
    return img


def emit(img, name):
    import struct
    W, H = img.size
    px = img.convert("RGBA").tobytes()
    runs = []
    i, total = 0, W * H
    while i < total:
        chunk = px[i * 4:i * 4 + 4]
        r, g, b, a = chunk
        bgra = b | (g << 8) | (r << 16) | (a << 24)
        n = 1
        while i + n < total and n < 0xFFFF and px[(i + n) * 4:(i + n) * 4 + 4] == chunk:
            n += 1
        runs.append((n, bgra))
        i += n
    assert sum(n for n, _ in runs) == total, "run lengths do not cover the image"

    bg = BG[2] | (BG[1] << 8) | (BG[0] << 16) | (0xFF << 24)
    blob = bytearray(struct.pack("<4sIIIII", b"SPCP", 2, W, H, len(runs), bg))
    for n, bgra in runs:
        blob += struct.pack("<HI", n, bgra)

    out = ROOT / "data" / "images" / f"no-phone-{name}.bin"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(blob)
    preview = HERE / f"no-phone-{name}.png"
    img.save(preview, optimize=True)
    print(f"  {name:9} {W}x{H}  {len(runs)} runs  {out.stat().st_size} bytes")


def main():
    if not EMIT.exists():
        sys.exit(f"missing {EMIT} — build it first (see docstring)")
    for orientation in LAYOUTS:
        emit(card(orientation), orientation)


if __name__ == "__main__":
    main()
