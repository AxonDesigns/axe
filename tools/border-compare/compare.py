#!/usr/bin/env python3
"""Compares Axe's per-side borders and corners with Chrome's, by eye and by numbers.

The boxes of tests/test_paint_golden.c's `paint_border_sides` case are written as HTML (the
same rects, sides, colors and radii, in the same order) and screenshotted by the pinned
chrome-headless-shell at device scale factors 1, 1.25 and 1.5. Each screenshot is put next
to Axe's golden of the same scale and an amplified difference:

    docs/images/border-compare@<scale>.png   Chrome | Axe | |difference| x 4

and the script prints how many pixels differ by more than a few levels. docs/styles.md
("Borders") records the result. Standard library only; run it as:

    python3 -I tools/layout-fixtures/fetch_chrome.py   # once
    python3 -I tools/border-compare/compare.py
"""
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "tools" / "layout-fixtures"))
import fetch_chrome  # noqa: E402

WIDTH, HEIGHT = 320, 160
SCALES = [(120, 1.0), (150, 1.25), (180, 1.5)]
GOLDENS = ROOT / "tests" / "goldens"
OUT = ROOT / "docs" / "images"


def rgb(r, g, b, a=1.0):
    return f"rgba({round(r * 255)}, {round(g * 255)}, {round(b * 255)}, {a})"


# The boxes of border_sides() in tests/test_paint_golden.c: (left, top, width, height,
# background, extra CSS). The page's background is the golden's root box.
BOXES = [
    (12, 12, 90, 60, rgb(0.95, 0.92, 0.85),
     f"border-width: 2px 12px 6px 20px; border-color: {rgb(0.15, 0.35, 0.75)}; border-radius: 24px"),
    (116, 12, 90, 60, rgb(1, 1, 1),
     f"border-width: 10px; border-color: {rgb(0.9, 0.2, 0.2)} {rgb(0.2, 0.7, 0.3)} {rgb(0.2, 0.3, 0.9)} "
     f"{rgb(0.9, 0.7, 0.1)}"),
    (220, 12, 90, 60, rgb(0.85, 0.95, 0.9),
     f"border-width: 6px; border-color: {rgb(0.3, 0.2, 0.5)} transparent; "
     "border-top-left-radius: 18px; border-bottom-right-radius: 18px"),
    (12, 88, 90, 60, rgb(0.9, 0.85, 0.95),
     f"border-width: 3px; border-color: {rgb(0.4, 0.1, 0.4)}; border-radius: 80px 40px 10px 0"),
    (116, 88, 90, 60, rgb(1, 1, 1),
     f"border-left: 16px solid {rgb(0.85, 0.35, 0.1)}; border-top: 1px solid {rgb(0.5, 0.5, 0.5)}; "
     "border-top-left-radius: 30px"),
    (220, 88, 90, 60, rgb(0.95, 0.95, 1),
     f"direction: rtl; border-inline-start: 14px solid {rgb(0.1, 0.5, 0.6)}; "
     f"border-inline-end: 2px solid {rgb(0.6, 0.1, 0.3)}; border-start-start-radius: 22px"),
]

PAGE = """<!doctype html>
<html><head><meta charset="utf-8"><style>
html, body {{ margin: 0; padding: 0; background: {bg}; overflow: hidden; }}
div {{ position: absolute; box-sizing: border-box; border-style: solid; border-width: 0; }}
</style></head><body>
{boxes}
</body></html>
"""


def page() -> str:
    boxes = "\n".join(f'<div style="left: {x}px; top: {y}px; width: {w}px; height: {h}px; '
                      f'background: {bg}; {css}"></div>' for x, y, w, h, bg, css in BOXES)
    return PAGE.format(bg=rgb(0.97, 0.97, 0.97), boxes=boxes)


# ---- Images: RGBA rows of bytes ------------------------------------------------------------

def read_qoi(path: pathlib.Path):
    data = path.read_bytes()
    if data[:4] != b"qoif":
        raise SystemExit(f"{path}: not QOI")
    w, h, channels = struct.unpack(">IIB", data[4:13])
    px, out, index, at = [0, 0, 0, 255], bytearray(), [[0, 0, 0, 0] for _ in range(64)], 14
    while len(out) < w * h * 4:
        b = data[at]
        at += 1
        if b == 0xFE:
            px = [data[at], data[at + 1], data[at + 2], px[3]]
            at += 3
        elif b == 0xFF:
            px = list(data[at:at + 4])
            at += 4
        elif b >> 6 == 0:
            px = list(index[b])
        elif b >> 6 == 1:
            px = [(px[0] + ((b >> 4) & 3) - 2) & 255, (px[1] + ((b >> 2) & 3) - 2) & 255,
                  (px[2] + (b & 3) - 2) & 255, px[3]]
        elif b >> 6 == 2:
            b2 = data[at]
            at += 1
            dg = (b & 63) - 32
            px = [(px[0] + dg - 8 + (b2 >> 4)) & 255, (px[1] + dg) & 255,
                  (px[2] + dg - 8 + (b2 & 15)) & 255, px[3]]
        else:
            for _ in range((b & 63) + 1):
                out += bytes(px)
            index[(px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) % 64] = list(px)
            continue
        index[(px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) % 64] = list(px)
        out += bytes(px)
    del channels
    return w, h, out[:w * h * 4]


def read_png(path: pathlib.Path):
    data = path.read_bytes()
    at, idat, w, h, ctype = 8, bytearray(), 0, 0, 0
    while at < len(data):
        n, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or interlace or ctype not in (2, 6):
                raise SystemExit(f"{path}: unsupported PNG")
        elif kind == b"IDAT":
            idat += body
        at += 12 + n
    bpp = 4 if ctype == 6 else 3
    raw, stride, rows = zlib.decompress(bytes(idat)), w * bpp, []
    prev = bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    out = bytearray()
    for line in rows:
        for x in range(w):
            out += line[x * bpp:x * bpp + 3] + (bytes([line[x * bpp + 3]]) if bpp == 4 else b"\xff")
    return w, h, out


def write_png(path: pathlib.Path, w: int, h: int, rgba: bytes):
    raw = b"".join(b"\x00" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
                     chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def compare(chrome_img, axe_img):
    (w, h, a), (_, _, b) = chrome_img, axe_img
    diff, worst, over = bytearray(), 0, 0
    for i in range(0, w * h * 4, 4):
        d = max(abs(a[i + k] - b[i + k]) for k in range(3))
        worst = max(worst, d)
        over += d > 8
        v = min(255, d * 4)
        diff += bytes([v, v, v, 255])
    side = bytearray()
    gap = bytes([255, 255, 255, 255]) * 4
    for y in range(h):
        row = slice(y * w * 4, (y + 1) * w * 4)
        side += a[row] + gap + b[row] + gap + diff[row]
    return side, w * 3 + 8, worst, over, w * h


def main() -> int:
    chrome = fetch_chrome.binary()
    if not chrome.exists():
        print("run tools/layout-fixtures/fetch_chrome.py first", file=sys.stderr)
        return 1
    OUT.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        html = pathlib.Path(tmp) / "borders.html"
        html.write_text(page(), encoding="utf-8")
        for scale_120, scale in SCALES:
            shot = pathlib.Path(tmp) / f"chrome{scale_120}.png"
            subprocess.run([str(chrome), "--headless", "--no-sandbox", "--disable-gpu", "--hide-scrollbars",
                            f"--window-size={WIDTH},{HEIGHT}", f"--force-device-scale-factor={scale}",
                            f"--screenshot={shot}", f"--user-data-dir={tmp}/profile", html.as_uri()],
                           capture_output=True, timeout=120, check=True)
            chrome_img = read_png(shot)
            axe_img = read_qoi(GOLDENS / f"paint_border_sides@{scale_120 * 100 // 120}.qoi")
            if chrome_img[:2] != axe_img[:2]:
                raise SystemExit(f"sizes differ at {scale}: Chrome {chrome_img[:2]}, Axe {axe_img[:2]}")
            side, sw, worst, over, total = compare(chrome_img, axe_img)
            out = OUT / f"border-compare@{scale_120 * 100 // 120}.png"
            write_png(out, sw, chrome_img[1], bytes(side))
            print(f"{scale}x: {over} of {total} pixels ({100 * over / total:.2f}%) differ by more than 8 levels, "
                  f"largest difference {worst}; wrote {out.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
