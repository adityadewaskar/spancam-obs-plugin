#!/usr/bin/env python3
"""Bake the installer artwork for all three platforms from one place.

Everything a user sees while INSTALLING is generated here, so the three installers
cannot drift apart visually:

  cmake/windows/resources/installer-wizard.bmp        164x314 side panel (Inno, modern)
  cmake/windows/resources/installer-wizard-small.bmp   55x55  header mark (Inno, modern)
  cmake/windows/resources/installer-icon.ico          the setup .exe's own icon
  cmake/macos/resources/installer/installer-background.png       productbuild bg, light
  cmake/macos/resources/installer/installer-background-dark.png  productbuild bg, dark
  cmake/macos/resources/installer/welcome.html                   first pane
  cmake/macos/resources/installer/conclusion.html                last pane

That directory is handed to productbuild as --resources, so the names referenced
from distribution.xml resolve. Nothing else in it is included unless referenced.

Inno accepts only BMP for the wizard images, hence no PNG there. The macOS
backgrounds are transparent PNGs so they float over whichever surface the installer
paints, in either appearance.

Linux has no installer UI at all — dpkg/apt are the installer — so its "branding" is
package metadata, set in cmake/linux/defaults.cmake rather than drawn here.

    python3 build-aux/branding/make-installer-art.py
"""
import pathlib
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent.parent
MARK = HERE / "spancam-mark.png"

WIN = ROOT / "cmake" / "windows" / "resources"
MAC = ROOT / "cmake" / "macos" / "resources" / "installer"

INK = (245, 245, 247)
DIM = (150, 150, 160)
DARK = (18, 18, 22)
ACCENT = (124, 92, 214)
BRAND = "Spancam for OBS"
TAG = "Your phone as a camera source"


def font(sz, bold=False):
    for p in ("/System/Library/Fonts/Supplemental/Helvetica.ttc",
              "/System/Library/Fonts/HelveticaNeue.ttc",
              "/System/Library/Fonts/Supplemental/Arial.ttf"):
        try:
            return ImageFont.truetype(p, sz, index=1 if bold else 0)
        except Exception:
            pass
    return ImageFont.load_default()


def rounded_mark(px):
    """The app mark, rounded the way it is drawn everywhere else."""
    m = Image.open(MARK).convert("RGBA").resize((px, px), Image.LANCZOS)
    mask = Image.new("L", (px, px), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, px - 1, px - 1], radius=int(px * 0.26), fill=255)
    out = Image.new("RGBA", (px, px), (0, 0, 0, 0))
    out.paste(m, (0, 0), mask)
    return out


def centred(d, text, fnt, y, w, fill):
    d.text(((w - d.textlength(text, font=fnt)) / 2, y), text, font=fnt, fill=fill)


def windows_wizard():
    """164x314 side panel. Dark, because the mark is drawn for dark ground and the
    white original made the installer look like a stock template."""
    W, H = 164, 314
    img = Image.new("RGB", (W, H), DARK)
    d = ImageDraw.Draw(img)
    # a soft accent wash behind the mark, flattened here since BMP has no alpha
    glow = Image.new("RGB", (W, H), DARK)
    gd = ImageDraw.Draw(glow)
    gd.ellipse([W // 2 - 62, 40, W // 2 + 62, 164], fill=(52, 40, 84))
    # blurred, or the ellipse reads as a hard-edged disc rather than a glow
    glow = glow.filter(ImageFilter.GaussianBlur(26))
    img = Image.blend(img, glow, 0.9)
    d = ImageDraw.Draw(img)
    mk = rounded_mark(72)
    img.paste(mk, ((W - 72) // 2, 56), mk)
    centred(d, "SPANCAM", font(15, True), 152, W, INK)
    centred(d, "for OBS", font(13), 172, W, DIM)
    d.line([28, 204, W - 28, 204], fill=(58, 58, 68), width=1)
    for i, line in enumerate(("Use your phone", "as a camera", "source in OBS")):
        centred(d, line, font(11), 218 + i * 16, W, DIM)
    img.save(WIN / "installer-wizard.bmp")
    print(f"  {(WIN / 'installer-wizard.bmp').relative_to(ROOT)}  {W}x{H}")


def windows_small():
    """55x55 header mark. Inno paints the modern header light, so this one keeps a
    light ground — a dark tile would sit on it as an obvious black square."""
    W = H = 55
    img = Image.new("RGB", (W, H), (255, 255, 255))
    mk = rounded_mark(43)
    img.paste(mk, ((W - 43) // 2, (H - 43) // 2), mk)
    img.save(WIN / "installer-wizard-small.bmp")
    print(f"  {(WIN / 'installer-wizard-small.bmp').relative_to(ROOT)}  {W}x{H}")


def windows_icon():
    """Multi-size .ico so Explorer, the taskbar and the UAC prompt each get a crisp
    size instead of one 256px image scaled down."""
    sizes = [16, 24, 32, 48, 64, 128, 256]
    base = rounded_mark(256)
    base.save(WIN / "installer-icon.ico", format="ICO",
              sizes=[(s, s) for s in sizes])
    print(f"  {(WIN / 'installer-icon.ico').relative_to(ROOT)}  {sizes}")


def mac_background(dark):
    """Transparent PNG, bottom-left aligned by distribution.xml. Transparent so it
    floats over whatever surface the installer paints in that appearance."""
    W, H = 300, 330
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    ink = (236, 236, 240) if dark else (28, 28, 34)
    dim = (150, 150, 160) if dark else (110, 110, 122)
    mk = rounded_mark(96)
    img.paste(mk, (34, H - 232), mk)
    d.text((34, H - 118), "SPANCAM", font=font(22, True), fill=ink)
    d.text((34, H - 90), "for OBS", font=font(19), fill=dim)
    d.text((34, H - 56), TAG, font=font(13), fill=dim)
    name = "installer-background-dark.png" if dark else "installer-background.png"
    img.save(MAC / name)
    print(f"  {(MAC / name).relative_to(ROOT)}  {W}x{H}  {'dark' if dark else 'light'}")


def main():
    if not MARK.exists():
        raise SystemExit(f"missing {MARK} — extract it with:\n"
                         f"  sips -s format png --resampleHeightWidth 512 512 "
                         f"cmake/macos/resources/spancam.icns --out {MARK}")
    WIN.mkdir(parents=True, exist_ok=True)
    MAC.mkdir(parents=True, exist_ok=True)
    windows_wizard()
    windows_small()
    windows_icon()
    mac_background(False)
    mac_background(True)


if __name__ == "__main__":
    main()
