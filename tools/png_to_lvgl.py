from pathlib import Path
from PIL import Image
import sys


def convert(source: Path, target: Path, symbol: str) -> None:
    image = Image.open(source).convert("RGBA")
    opaque = len(sys.argv) == 5 and sys.argv[4] == "--opaque"
    values = []
    for red, green, blue, alpha in image.getdata():
        color = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)
        values.extend((color >> 8, color & 0xFF))
        if not opaque:
            values.append(alpha)

    lines = []
    for index in range(0, len(values), 24):
        lines.append("    " + ", ".join(f"0x{value:02X}" for value in values[index:index + 24]) + ",")

    target.write_text(
        '#include "lvgl.h"\n\n'
        f"static const uint8_t {symbol}_data[] = {{\n"
        + "\n".join(lines)
        + "\n};\n\n"
        f"const lv_img_dsc_t {symbol} = {{\n"
        f"    .header.cf = {'LV_IMG_CF_TRUE_COLOR' if opaque else 'LV_IMG_CF_TRUE_COLOR_ALPHA'},\n"
        f"    .header.w = {image.width},\n"
        f"    .header.h = {image.height},\n"
        f"    .data_size = sizeof({symbol}_data),\n"
        f"    .data = {symbol}_data,\n"
        "};\n",
        encoding="ascii",
    )


if __name__ == "__main__":
    if len(sys.argv) not in (4, 5):
        raise SystemExit("usage: png_to_lvgl.py input.png output.c symbol [--opaque]")
    convert(Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3])
