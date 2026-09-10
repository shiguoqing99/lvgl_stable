#!/usr/bin/env python3
"""把 TTF 转成 LVGL 8.4 可用的 4bpp C 字库。

设计要点
--------
ESP32-C3 上的 LVGL 8.4 中，任何 `transform_zoom != 256` 的对象都会被判定为
`LV_LAYER_TYPE_TRANSFORM`，必须一次性申请整块离屏图层（不能像 SIMPLE 图层那样
分块 `CAN_SUBDIVIDE`）。内存不足时 lv_refr.c 会直接 `return`，对象被静默跳过。
因此本项目**禁止用 transform_zoom 缩放文字**，改为按用途生成多档真实字号。

生成的格式与 LVGL 8.4 `lv_font_fmt_txt` 完全对齐：
  * bitmap 为 4bpp 紧凑排列，每个字节的高 4 位是先出现的像素
  * `adv_w` 为 8.4 定点（真实像素 * 16）
  * `ofs_y = -(基线下方的墨迹深度)`，配合 `bitmap_top = y + (line_height - base_line) - box_h - ofs_y`
  * cmap 分两段：ASCII 连续段用 FORMAT0_TINY，CJK 稀疏段用 SPARSE_TINY
    （注意 SPARSE_TINY 的 unicode_list 存的是 `codepoint - range_start` 相对值）

用法
----
    python gen_lvgl_font.py <ttf> <size> <line_height> <base_line> <输出.c> <符号名> [源文件...]
"""
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# CJK 常用标点，字库里常驻，避免以后加文案时缺字
EXTRA_CHARS = "，。？！、：；…—·“”‘’（）《》℃☆★"


def collect_chars(sources):
    """从 C 源文件的字符串字面量里收集所有需要的字符。"""
    chars = set(EXTRA_CHARS)
    for path in sources:
        text = Path(path).read_text(encoding="utf-8", errors="replace")
        for literal in re.findall(r'"((?:[^"\\]|\\.)*)"', text):
            literal = re.sub(r"\\[0-7]{1,3}|\\.", "", literal)  # 去掉转义与 %d 之类
            for ch in literal:
                if 0x20 <= ord(ch) != 0x7F:
                    chars.add(ch)
    return chars


def render_glyph(font, ch, base_line):
    """渲染单个字符，返回 (bitmap_4bpp, adv_w, box_w, box_h, ofs_x, ofs_y)。"""
    x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
    above = max(0, -y0)          # 基线上方的墨迹高度
    below = max(0, y1)           # 基线下方的墨迹深度
    box_w = x1 - x0
    box_h = above + below
    if box_w <= 0 or box_h <= 0:
        return None

    image = Image.new("L", (box_w, box_h), 0)
    # anchor='ls' 表示锚点在基线左端；把基线放在第 above 行，墨迹正好填满整块
    ImageDraw.Draw(image).text((-x0, above), ch, font=font, anchor="ls", fill=255)

    levels = [(v * 15 + 127) // 255 for v in image.tobytes()]
    packed = bytearray()
    for i in range(0, len(levels), 2):
        low = levels[i + 1] if i + 1 < len(levels) else 0
        packed.append((levels[i] << 4) | low)

    adv_w = int(round(font.getlength(ch) * 16))
    return bytes(packed), adv_w, box_w, box_h, x0, -below


def build_source(name, glyphs, cps, size, line_height, base_line):
    """glyphs[0] 是保留的空字形；cps[i] 对应 glyphs[i] 的码点。"""
    bitmap = bytearray()
    lines = []
    for packed, adv_w, box_w, box_h, ofs_x, ofs_y in glyphs:
        lines.append(
            f"    {{.bitmap_index = {len(bitmap)}, .adv_w = {adv_w}, "
            f".box_w = {box_w}, .box_h = {box_h}, .ofs_x = {ofs_x}, .ofs_y = {ofs_y}}},"
        )
        bitmap += packed

    # ASCII 连续段：0x20~0x7E
    ascii_cps = [c for c in cps if 0x20 <= c <= 0x7E]
    ascii_start = min(ascii_cps) if ascii_cps else 0x20
    ascii_len = (max(ascii_cps) - ascii_start + 1) if ascii_cps else 0
    # 注意：必须排除 glyph 0 的占位码点 0，否则整张表会错位一格，
    # 表现为"字能显示但每个都变成码点表里的下一个字"
    cjk_cps = [c for c in cps if c > 0x7E]
    cjk_start = min(cjk_cps) if cjk_cps else 0
    cjk_len = (max(cjk_cps) - cjk_start + 1) if cjk_cps else 1

    # ASCII 段从 glyph 1 开始，因此 ASCII 必须严格连续
    ascii_glyph_start = 1
    cjk_glyph_start = 1 + ascii_len

    if len(glyphs) != 1 + ascii_len + len(cjk_cps):
        raise SystemExit(
            f"字形数量不一致：glyphs={len(glyphs)} 应为 {1 + ascii_len + len(cjk_cps)} "
            f"(1 个保留 + {ascii_len} 个 ASCII + {len(cjk_cps)} 个 CJK)"
        )

    cmaps = []
    if ascii_len:
        cmaps.append(
            "    {\n"
            f"        .range_start = {ascii_start}, .range_length = {ascii_len}, "
            f".glyph_id_start = {ascii_glyph_start},\n"
            "        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, "
            ".type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY\n"
            "    },"
        )
    if cjk_cps:
        entries = ", ".join(f"0x{c - cjk_start:04X}" for c in cjk_cps)
        wrapped = []
        row = []
        for i, token in enumerate(entries.split(", ")):
            row.append(token)
            if len(row) == 8 or i == len(cjk_cps) - 1:
                wrapped.append("        " + ", ".join(row) + ",")
                row = []
        cmaps.append(
            "    {\n"
            f"        .range_start = {cjk_start}, .range_length = {cjk_len}, "
            f".glyph_id_start = {cjk_glyph_start},\n"
            f"        .unicode_list = {name}_unicode_list, .glyph_id_ofs_list = NULL, "
            f".list_length = {len(cjk_cps)},\n"
            "        .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n"
            "    },"
        )
        unicode_list = (
            f"static const uint16_t {name}_unicode_list[] = {{\n"
            + "\n".join(wrapped)
            + "\n};"
        )
    else:
        unicode_list = ""

    def hex_rows(data, per_line=16):
        out = []
        for i in range(0, len(data), per_line):
            out.append("    " + ", ".join(f"0x{b:02X}" for b in data[i:i + per_line]) + ",")
        return "\n".join(out)

    return f"""#include "lvgl.h"

/* 由 tools/gen_lvgl_font.py 生成：size={size} line_height={line_height} base_line={base_line}
 * 请勿手工编辑；需要新字时改源文件里的文案后重新运行脚本。
 */

static const uint8_t {name}_glyph_bitmap[] = {{
{hex_rows(bitmap)}
}};

static const lv_font_fmt_txt_glyph_dsc_t {name}_glyph_dsc[] = {{
{chr(10).join(lines)}
}};

{unicode_list}

static const lv_font_fmt_txt_cmap_t {name}_cmaps[] = {{
{chr(10).join(cmaps)}
}};

static lv_font_fmt_txt_glyph_cache_t {name}_cache;

static const lv_font_fmt_txt_dsc_t {name}_dsc = {{
    .glyph_bitmap = {name}_glyph_bitmap,
    .glyph_dsc = {name}_glyph_dsc,
    .cmaps = {name}_cmaps,
    .kern_dsc = NULL,
    .kern_scale = 0,
    .cmap_num = {len(cmaps)},
    .bpp = 4,
    .kern_classes = 0,
    .bitmap_format = 0,
    .cache = &{name}_cache
}};

const lv_font_t {name} = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {line_height},
    .base_line = {base_line},
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = -2,
    .underline_thickness = 1,
    .dsc = &{name}_dsc,
    .fallback = NULL,
    .user_data = NULL,
}};
"""


def main():
    if len(sys.argv) < 7:
        raise SystemExit(
            "usage: gen_lvgl_font.py <ttf> <size> <line_height> <base_line> <output.c> <symbol> [sources...]"
        )
    ttf, size, line_height, base_line, output, name = sys.argv[1:7]
    sources = sys.argv[7:]
    size, line_height, base_line = int(size), int(line_height), int(base_line)

    font = ImageFont.truetype(ttf, size)
    chars = collect_chars(sources) if sources else set(EXTRA_CHARS)
    # ASCII 必须连续，补齐 0x20~0x7E
    chars |= {chr(c) for c in range(0x20, 0x7F)}

    cps = sorted({ord(c) for c in chars})
    ascii_cps = [c for c in cps if 0x20 <= c <= 0x7E]
    ascii_start = min(ascii_cps)
    ascii_len = max(ascii_cps) - ascii_start + 1
    if len(ascii_cps) != ascii_len:
        raise SystemExit("ASCII 段必须连续")

    # glyph 0 保留，随后是 ASCII 连续段，再后是 CJK 稀疏段
    ordered = [chr(c) for c in range(ascii_start, ascii_start + ascii_len)]
    ordered += [chr(c) for c in sorted(c for c in cps if c > 0x7E)]

    glyphs = [(b"", 0, 0, 0, 0, 0)]  # glyph 0：LVGL 保留的空字形
    glyph_cps = [0]
    for ch in ordered:
        result = render_glyph(font, ch, base_line)
        if result is None:
            result = (b"", 0, 0, 0, 0, 0)
        packed, adv_w, box_w, box_h, ofs_x, ofs_y = result
        glyphs.append((packed, adv_w, box_w, box_h, ofs_x, ofs_y))
        glyph_cps.append(ord(ch))

    Path(output).write_text(
        build_source(name, glyphs, glyph_cps, size, line_height, base_line),
        encoding="utf-8",
    )
    total = sum(len(g[0]) for g in glyphs)
    print(f"{output}: 字形 {len(glyphs)} 个，点阵 {total} 字节，字号 {size}px")


if __name__ == "__main__":
    main()
