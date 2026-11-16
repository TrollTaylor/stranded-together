"""Build the Stranded Together launcher's icon and in-window emblem from the project logo.

Reads docs/readme/logo.png (1535x1024 RGBA, transparent background: two amber pods
joined by a cable, with black "STRANDED TOGETHER" lettering and drip lines below) and
writes
  native/launcher/resources/st_launcher.ico  the app icon, 16 20 24 32 40 48 64 96 128 256 px
  native/launcher/resources/st_logo.png      the emblem (pods + cable only), EMBLEM_WIDTH px wide

The black lettering is invisible on the launcher's dark UI, so it is cut away: the
script finds the pods by their alpha. It takes the alpha row profile (pixels with
alpha > ALPHA_CUT), splits it into vertical runs separated by fully transparent rows,
and keeps the first (top) run, which must be followed by at least one more run (the
lettering). The emblem is that run's alpha bounding box plus a small margin. The
window draws the title "STRANDED TOGETHER" itself, as spaced ImGui text.

Every icon size is the emblem (at SINGLE_POD_UP_TO px and below: the left pod alone,
cut at the thinnest column of the cable) centred on a square canvas and scaled with a
premultiplied-alpha Lanczos filter (Pillow), plus a light unsharp mask at the small
sizes. The 256 px image is PNG-compressed, the others are 32-bit bitmaps with a mask
(the usual icon layout, which every part of Windows reads).

Python 3.8 or later with Pillow. Deterministic for a given Pillow version; --check
compares pixels (not PNG bytes, which can differ between zlib builds) and allows a
difference of at most CHECK_TOLERANCE per channel, so a different Pillow release does
not fail the check over rounding.

Usage: python tools/launcher/make_launcher_assets.py [--check] [--preview DIR]
  (no option)    regenerate both files
  --check        regenerate in memory; exit 1 if the committed files differ
  --preview DIR  also write each size as icon-<size>.png and contact sheets of all
                 sizes on a light and a dark background
"""
import argparse
import io
import os
import struct
import sys

try:
    from PIL import Image, ImageChops, ImageFilter
except ImportError:  # pragma: no cover
    sys.exit('make_launcher_assets.py needs Pillow (python -m pip install Pillow)')

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
SOURCE = os.path.join(ROOT, 'docs', 'readme', 'logo.png')
OUT_DIR = os.path.join(ROOT, 'native', 'launcher', 'resources')
ICO_PATH = os.path.join(OUT_DIR, 'st_launcher.ico')
EMBLEM_PATH = os.path.join(OUT_DIR, 'st_logo.png')

ICON_SIZES = (256, 128, 96, 64, 48, 40, 32, 24, 20, 16)
SHARPEN = {256: 0, 128: 30, 96: 40, 64: 50, 48: 60, 40: 60, 32: 70, 24: 70, 20: 70, 16: 70}  # unsharp percent
EMBLEM_WIDTH = 384
ALPHA_CUT = 32          # alpha above this counts as part of the art
MARGIN = 0.02           # emblem margin, as a fraction of the crop's larger side
ICON_FILL = 0.98        # the emblem's larger side covers this fraction of the icon
SINGLE_POD_UP_TO = 48   # icon sizes at or below this show one pod (the pair is a thin strip there)
CHECK_TOLERANCE = 2


# ---- The pods region -----------------------------------------------------------
def pods_box(image):
    """(left, top, right, bottom) of the top art run (the pods and the cable)."""
    alpha = image.getchannel('A').point(lambda v: 255 if v > ALPHA_CUT else 0)
    w, h = image.size
    rows = []
    for y in range(h):
        rows.append(alpha.crop((0, y, w, y + 1)).getbbox() is not None)
    runs = []
    start = None
    for y, filled in enumerate(rows):
        if filled and start is None:
            start = y
        elif not filled and start is not None:
            runs.append((start, y))
            start = None
    if start is not None:
        runs.append((start, h))
    # Ignore specks: a run must be at least 2% of the height.
    runs = [r for r in runs if r[1] - r[0] >= h * 0.02]
    if len(runs) < 2:
        raise SystemExit('logo.png changed: expected the pods above the lettering (two separate alpha runs), '
                         'found %d; review pods_box() before regenerating' % len(runs))
    top, bottom = runs[0]
    box = alpha.crop((0, top, w, bottom)).getbbox()
    left, right = box[0], box[2]
    pad = int(max(right - left, bottom - top) * MARGIN + 0.5)
    return (max(0, left - pad), max(0, top - pad), min(w, right + pad), min(h, bottom + pad))


def left_pod(pods):
    """The left pod alone: the art left of the thinnest column (the cable) in the middle
    of the pods crop, cropped to its alpha bounding box."""
    alpha = pods.getchannel('A').point(lambda v: 255 if v > ALPHA_CUT else 0)
    w, h = pods.size
    counts = alpha.resize((w, 1), Image.BOX)   # mean alpha per column
    lo, hi = int(w * 0.30), int(w * 0.50)
    cut = min(range(lo, hi), key=lambda x: counts.getpixel((x, 0)))
    box = alpha.crop((0, 0, cut, h)).getbbox()
    pad = int(max(box[2] - box[0], box[3] - box[1]) * MARGIN + 0.5)
    return pods.crop((max(0, box[0] - pad), max(0, box[1] - pad), min(cut, box[2] + pad), min(h, box[3] + pad)))


def resize_rgba(image, size):
    """Lanczos resize in premultiplied alpha (no dark fringes at transparent edges)."""
    return image.convert('RGBa').resize(size, Image.LANCZOS).convert('RGBA')


def clean(image):
    """Fully transparent pixels get zero colour (stable bytes, no hidden colour)."""
    px = image.load()
    w, h = image.size
    for y in range(h):
        for x in range(w):
            if px[x, y][3] == 0:
                px[x, y] = (0, 0, 0, 0)
    return image


def render(source):
    pods = source.crop(pods_box(source))
    pw, ph = pods.size
    emblem = clean(resize_rgba(pods, (EMBLEM_WIDTH, int(ph * EMBLEM_WIDTH / pw + 0.5))))
    single = left_pod(pods)
    icons = {}
    for size in ICON_SIZES:
        art = single if size <= SINGLE_POD_UP_TO else pods
        aw, ah = art.size
        k = size * ICON_FILL / max(aw, ah)
        sw, sh = max(1, int(aw * k + 0.5)), max(1, int(ah * k + 0.5))
        scaled = resize_rgba(art, (sw, sh))
        if SHARPEN[size]:
            rgb = scaled.convert('RGB').filter(ImageFilter.UnsharpMask(radius=0.8, percent=SHARPEN[size], threshold=1))
            rgb.putalpha(scaled.getchannel('A'))
            scaled = rgb
        canvas = Image.new('RGBA', (size, size), (0, 0, 0, 0))
        canvas.paste(scaled, ((size - sw) // 2, (size - sh) // 2))
        icons[size] = clean(canvas)
    return emblem, icons


# ---- .ico ----------------------------------------------------------------------
def encode_png(image):
    out = io.BytesIO()
    image.save(out, format='PNG', optimize=True)
    return out.getvalue()


def encode_dib(image):
    """A 32-bit icon bitmap: BITMAPINFOHEADER (height doubled), BGRA rows bottom-up, then
    the 1-bit AND mask (1 = transparent where alpha < 128), rows padded to 4 bytes."""
    size = image.size[0]
    px = image.load()
    mask_stride = (size + 31) // 32 * 4
    xor = bytearray()
    mask = bytearray()
    for y in range(size - 1, -1, -1):
        bits = bytearray(mask_stride)
        for x in range(size):
            r, g, b, a = px[x, y]
            xor += bytes((b, g, r, a))
            if a < 128:
                bits[x // 8] |= 0x80 >> (x % 8)
        mask += bits
    header = struct.pack('<IiiHHIIiiII', 40, size, size * 2, 1, 32, 0, len(xor) + len(mask), 0, 0, 0, 0)
    return header + bytes(xor) + bytes(mask)


def decode_dib(size, data):
    bi_size, w, h2, planes, bits, compression = struct.unpack('<IiiHHI', data[:20])
    if (bi_size, w, h2, planes, bits, compression) != (40, size, size * 2, 1, 32, 0):
        raise ValueError('unexpected bitmap header for %d px' % size)
    image = Image.new('RGBA', (size, size))
    px = image.load()
    for y in range(size):
        row = 40 + (size - 1 - y) * size * 4
        for x in range(size):
            b, g, r, a = data[row + x * 4:row + x * 4 + 4]
            px[x, y] = (r, g, b, a)
    return image


def encode_ico(icons):
    blobs = [(size, encode_png(icons[size]) if size >= 256 else encode_dib(icons[size])) for size in ICON_SIZES]
    header = struct.pack('<HHH', 0, 1, len(blobs))
    offset = 6 + 16 * len(blobs)
    entries = b''
    for size, blob in blobs:
        dim = 0 if size >= 256 else size
        entries += struct.pack('<BBBBHHII', dim, dim, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    return header + entries + b''.join(blob for _, blob in blobs)


def decode_ico(data):
    _, kind, count = struct.unpack('<HHH', data[:6])
    if kind != 1:
        raise ValueError('not an icon file')
    icons = {}
    for i in range(count):
        w, _, _, _, _, _, length, offset = struct.unpack('<BBBBHHII', data[6 + 16 * i:22 + 16 * i])
        size = w or 256
        blob = data[offset:offset + length]
        if blob[:8] == b'\x89PNG\r\n\x1a\n':
            icons[size] = Image.open(io.BytesIO(blob)).convert('RGBA')
        else:
            icons[size] = decode_dib(size, blob)
    return icons


# ---- Check and previews --------------------------------------------------------
def same_pixels(a, b):
    if a.size != b.size:
        return False
    extrema = ImageChops.difference(a.convert('RGBA'), b.convert('RGBA')).getextrema()
    return all(high <= CHECK_TOLERANCE for _, high in extrema)


def write_previews(directory, icons, emblem):
    os.makedirs(directory, exist_ok=True)
    for size in ICON_SIZES:
        icons[size].save(os.path.join(directory, 'icon-%d.png' % size))
    for name, background in (('light', (243, 243, 243, 255)), ('dark', (16, 19, 24, 255))):
        pad = 12
        sizes = sorted(ICON_SIZES)
        sheet = Image.new('RGBA', (pad + sum(s + pad for s in sizes), 256 + 2 * pad), background)
        x = pad
        for s in sizes:
            sheet.alpha_composite(icons[s], (x, 256 + pad - s))
            x += s + pad
        sheet.save(os.path.join(directory, 'sheet-%s.png' % name))
        e = Image.new('RGBA', (emblem.size[0] + 2 * pad, emblem.size[1] + 2 * pad), background)
        e.alpha_composite(emblem, (pad, pad))
        e.save(os.path.join(directory, 'emblem-%s.png' % name))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--check', action='store_true', help='exit 1 if the committed files differ from a fresh render')
    parser.add_argument('--preview', help='also write each icon size and contact sheets here')
    args = parser.parse_args()
    source = Image.open(SOURCE).convert('RGBA')
    emblem, icons = render(source)
    if args.preview:
        write_previews(args.preview, icons, emblem)
    if args.check:
        problems = []
        if not os.path.isfile(ICO_PATH) or not os.path.isfile(EMBLEM_PATH):
            problems.append('st_launcher.ico or st_logo.png is missing')
        else:
            committed = decode_ico(open(ICO_PATH, 'rb').read())
            if sorted(committed) != sorted(ICON_SIZES):
                problems.append('st_launcher.ico sizes %s, expected %s' % (sorted(committed), sorted(ICON_SIZES)))
            for size in ICON_SIZES:
                if size in committed and not same_pixels(committed[size], icons[size]):
                    problems.append('st_launcher.ico %d px differs from the logo render' % size)
            if not same_pixels(Image.open(EMBLEM_PATH).convert('RGBA'), emblem):
                problems.append('st_logo.png differs from the logo render')
        for line in problems:
            print('FAIL: ' + line)
        print('launcher assets: %s' % ('out of date, run tools/launcher/make_launcher_assets.py' if problems else 'match the logo'))
        return 1 if problems else 0
    os.makedirs(OUT_DIR, exist_ok=True)
    with open(ICO_PATH, 'wb') as f:
        f.write(encode_ico(icons))
    with open(EMBLEM_PATH, 'wb') as f:
        f.write(encode_png(emblem))
    print('wrote st_launcher.ico (%s px) and st_logo.png (%dx%d)' %
          (' '.join(str(s) for s in sorted(ICON_SIZES)), emblem.size[0], emblem.size[1]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
