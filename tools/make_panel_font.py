"""Build the panel's packed bitmap font from an OFL Noto Sans SC variable font.

Usage: python tools/make_panel_font.py path/to/NotoSansSC.ttf
Requires Pillow. Normal builds use the committed binary and do not run this script.
"""
import argparse
from pathlib import Path
import struct

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
ASCII = set(range(0x20, 0x7F))


def ui_chars(*files):
    return {
        ord(ch)
        for file in files
        for ch in (ROOT / "main" / file).read_text(encoding="utf-8")
        if 0x80 <= ord(ch) < 0x10000
    }


def common_han():
    characters = set()
    for lead in range(0xB0, 0xD8):
        for trail in range(0xA1, 0xFF):
            text = bytes((lead, trail)).decode("gb2312", errors="ignore")
            if text:
                characters.add(ord(text))
    return characters


def packed_glyph(code, size, font):
    width = size if code >= 0x80 else (15 if size == 24 else 10)
    ch = chr(code)
    left, _, right, _ = font.getbbox(ch, anchor="ls")
    ink = right - left
    scratch_width = max(width, ink)
    image = Image.new("L", (scratch_width, size))
    draw = ImageDraw.Draw(image)
    draw.text(((scratch_width - ink) // 2 - left, size - (4 if size == 24 else 3)),
              ch, font=font, fill=255, anchor="ls")
    if scratch_width > width:
        image = image.resize((width, size), Image.Resampling.LANCZOS)
    cell = Image.new("L", (size, size))
    cell.paste(image, (0, 0))
    levels = [(sample + 42) // 85 for sample in cell.tobytes()]
    return bytes(sum(levels[i + n] << (2 * n) for n in range(4))
                 for i in range(0, len(levels), 4))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("font", type=Path)
    path = parser.parse_args().font
    body = ASCII | common_han() | ui_chars("front_panel_view.c", "front_panel.c") | {0x2026}
    headings = ASCII | ui_chars("front_panel_view.c") | {0x2026}
    data = bytearray(struct.pack("<HH", len(body), len(headings)))
    for size, codes, weight in ((16, body, 480), (24, headings, 650)):
        font = ImageFont.truetype(str(path), size - 1)
        font.set_variation_by_axes([weight])
        for code in sorted(codes):
            data += struct.pack("<H", code) + packed_glyph(code, size, font)
    (ROOT / "main" / "panel_assets" / "panel_font.bin").write_bytes(data)


if __name__ == "__main__":
    main()
