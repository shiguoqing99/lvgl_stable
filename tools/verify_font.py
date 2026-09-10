#!/usr/bin/env python3
"""离线验证 LVGL 8.4 字库：完全复刻 get_glyph_dsc_id() 的查表路径，
把查到的字模打成 ASCII 图，用来确认"码点 -> 字形"没有错位。

用法
----
    python verify_font.py <font.c> <符号名> [要检查的字符串]

不传字符串时默认检查界面常用字。
"""
import re
import sys
from pathlib import Path

DEFAULT_TEXT = "今天是我们结婚的第天年月日星期"


class Font:
    def __init__(self, path, name, prefix=None, ulist_name=None):
        src = Path(path).read_text(encoding="utf-8", errors="replace")
        self.name = name
        # 老字库（如官方转换器产出的 my_font.c）用无前缀的全局数组名
        def arr(base):
            return f"{prefix}_{base}" if prefix else base

        prefix = prefix if prefix is not None else (
            name if re.search(rf"{name}_glyph_bitmap", src) else ""
        )
        ulist_name = ulist_name or arr("unicode_list")
        self.bitmap = bytes(
            int(v, 16)
            for v in re.findall(
                r"0x[0-9A-Fa-f]+",
                re.search(rf"{arr('glyph_bitmap')}\[\] =\s*\{{(.*?)\n\}};", src, re.S).group(1),
            )
        )
        self.glyphs = [
            tuple(map(int, m))
            for m in re.findall(
                r"\{\.bitmap_index = (-?\d+), \.adv_w = (-?\d+), \.box_w = (-?\d+), "
                r"\.box_h = (-?\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}",
                re.search(rf"{arr('glyph_dsc')}\[\] =\s*\{{(.*?)\n\}};", src, re.S).group(1),
            )
        ]
        # 官方转换器会按 cmap 序号命名：unicode_list_1 / _2 ...
        m = None
        for cand in [ulist_name, arr("unicode_list")] + [f"unicode_list_{i}" for i in range(1, 8)]:
            m = re.search(rf"{cand}\[\] =\s*\{{(.*?)\n\}};", src, re.S)
            if m:
                break
        self.ulist = [int(v, 16) for v in re.findall(r"0x[0-9A-Fa-f]+", m.group(1))] if m else []
        cmaps_src = re.search(rf"{arr('cmaps')}\[\] =\s*\{{(.*?)\n\}};", src, re.S).group(1)
        self.cmaps = []
        for blk in re.findall(r"\.range_start = (\d+), \.range_length = (\d+), "
                              r"\.glyph_id_start = (\d+),(.*?)(?=\}|$)", cmaps_src, re.S):
            rs, rl, gs, rest = blk
            has_list = "NULL" not in re.search(r"\.unicode_list = ([^,]+),", rest).group(1)
            typ = re.search(r"\.type = (\w+)", rest).group(1)
            self.cmaps.append({
                "range_start": int(rs),
                "range_length": int(rl),
                "glyph_id_start": int(gs),
                "sparse": "SPARSE" in typ,
                "has_list": has_list,
                "type": typ,
            })

    def lookup(self, letter):
        """与 lv_font_fmt_txt.c 的 get_glyph_dsc_id() 完全一致。"""
        for c in self.cmaps:
            rcp = letter - c["range_start"]
            if rcp > c["range_length"]:
                continue
            if not c["sparse"]:
                return c["glyph_id_start"] + rcp
            if rcp in self.ulist:
                return c["glyph_id_start"] + self.ulist.index(rcp)
            return 0
        return 0

    def art(self, gid):
        bi, aw, bw, bh, ox, oy = self.glyphs[gid]
        rows = []
        for y in range(bh):
            line = ""
            for x in range(bw):
                bit = (y * bw + x) * 4
                byte = self.bitmap[bi + bit // 8]
                nib = (byte >> 4) if (bit % 8) == 0 else (byte & 0x0F)
                line += " .:-=+*#@"[min(8, nib * 8 // 16)] if nib else " "
            rows.append(line)
        return rows


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: verify_font.py <font.c> <symbol> [text]")
    path, name = sys.argv[1], sys.argv[2]
    text = sys.argv[3] if len(sys.argv) > 3 else DEFAULT_TEXT

    f = Font(path, name)
    print(f"=== {name}: 字形 {len(f.glyphs)}，unicode_list {len(f.ulist)}，cmap {len(f.cmaps)}")
    for c in f.cmaps:
        print(f"    range_start={c['range_start']} range_length={c['range_length']} "
              f"glyph_id_start={c['glyph_id_start']} {c['type']}")

    missing = [ch for ch in text if f.lookup(ord(ch)) == 0]
    if missing:
        print("!! 缺字:", "".join(missing))

    for ch in text:
        gid = f.lookup(ord(ch))
        if not gid:
            print(f"--- '{ch}' U+{ord(ch):04X} -> 无字形")
            continue
        bi, aw, bw, bh, ox, oy = f.glyphs[gid]
        print(f"--- '{ch}' U+{ord(ch):04X} -> gid={gid} box={bw}x{bh} adv={aw/16:.2f}")
        for row in f.art(gid):
            print("    |" + row + "|")


if __name__ == "__main__":
    main()
