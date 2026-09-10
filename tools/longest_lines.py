#!/usr/bin/env python3
"""统计每日语录 / 特殊节日语录里最长的几句，并按 13px 字库算出实际渲染宽度。

"最长"有两种口径，UI 上真正有风险的是**渲染宽度**而不是字数：
语录里有 "WiFi" 这类半角字符，字数多不一定更宽。
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from preview_ui import LvglFont  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
MAIN = ROOT / "main" / "lvgl_stable.c"
LUNAR = ROOT / "main" / "lunar_special.c"
WIDTH = 240

main_src = MAIN.read_text(encoding="utf-8")
lunar_src = LUNAR.read_text(encoding="utf-8")

daily = re.findall(r'"([^"]*)"', re.search(r"daily_lines\[\] = \{(.*?)\n\};", main_src, re.S).group(1))
special = re.findall(r'\{"[\d-]+",\s*"([^"]*)"\}', main_src)
lunar = re.findall(r'\{"[\d-]+",\s*"([^"]*)"\}', lunar_src)

f13 = LvglFont(str(ROOT / "main" / "my_font_13.c"), "my_font_13")

rows = []
seen = set()
for group, texts in [("日常语录", daily), ("固定/公历节日", special), ("农历节日", lunar)]:
    for t in texts:
        if t in seen:
            continue
        seen.add(t)
        rows.append((group, t, len(t), f13.measure(t)))

rows.sort(key=lambda r: -r[3])
print(f"不同文案共 {len(rows)} 条（去重后），屏幕可用宽度 {WIDTH}px\n")
print(f"{'排名':<4}{'来源':<14}{'字数':<6}{'渲染宽度':<10}文案")
for i, (group, t, n, w) in enumerate(rows[:8], 1):
    print(f"{i:<5}{group:<14}{n:<7}{w:6.1f}px   {t}")

wmax = max(r[3] for r in rows)
nmax = max(r[2] for r in rows)
print(f"\n渲染最宽: {wmax:.1f}px（余量 {WIDTH - wmax:.1f}px，占屏 {wmax / WIDTH * 100:.1f}%）")
print(f"字数最多: {nmax} 字 —— " + " / ".join(t for g, t, n, w in rows if n == nmax))
over = [t for g, t, n, w in rows if w > WIDTH]
print("超出屏宽的条数:", len(over) if over else "0（无溢出风险）")
