#!/usr/bin/env python3
"""检查界面上所有可能出现的中英文案是否都已被字库收录。

字库生成时是从源文件的字符串字面量里收集字符的。lunar_special.c 曾被破坏成
"???" 后才生成 13px 字库，因此节日文案的汉字有整批缺字的风险。缺字在屏幕上
表现为空白，不易察觉，所以必须在每次改文案 / 重建字库后跑一次。

用法
----
    python tools/check_font_coverage.py
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from verify_font import Font  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
MAIN = ROOT / "main" / "lvgl_stable.c"
LUNAR = ROOT / "main" / "lunar_special.c"

# (字库文件, 符号名, 它负责渲染的文案来源)
TARGETS = [
    ("my_font_13.c", "my_font_13", ["blessing"]),
    ("my_font_18.c", "my_font_18", ["date", "marriage"]),
    ("my_font.c", "my_font", ["days"]),
]


DIGITS = [str(i) for i in range(10)]
WEEKDAYS = ["星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"]


def collect_strings():
    """收集真正会被渲染到屏幕上的文案。

    注意：只能取运行期会传给 lv_label_set_text 的字符串，不能从代码注释里抓
    ——注释里的中文（"墨迹中心""描边"之类）本来就不该进字库，抓了会误报缺字。
    """
    main_src = MAIN.read_text(encoding="utf-8")
    lunar_src = LUNAR.read_text(encoding="utf-8")

    daily = re.findall(r'"([^"]*)"', re.search(r"daily_lines\[\] = \{(.*?)\n\};", main_src, re.S).group(1))
    special = re.findall(r'\{"[\d-]+",\s*"([^"]*)"\}', main_src)
    lunar = re.findall(r'\{"[\d-]+",\s*"([^"]*)"\}', lunar_src)

    return {
        # 语录行（13px）：日常 31 条 + 公历特殊日 + 农历节日 450 条
        "blessing": daily + special + lunar,
        # 日期行（18px）："%d年%02d月%02d日" + 星期
        "date": ["年月日"] + DIGITS + WEEKDAYS,
        # 结婚提示（18px）
        "marriage": ["今天是我们结婚的"],
        # 天数（25px）：「第 N 天」+ 冒号时间分隔
        "days": ["第", "天", ":"] + DIGITS,
    }


def main():
    pools = collect_strings()
    print(f"文案统计: 语录 {len(pools['blessing'])} 条（含农历 {len(pools['blessing']) - 31 - 7} 条）")

    bad = False
    for filename, symbol, keys in TARGETS:
        font = Font(str(ROOT / "main" / filename), symbol)
        chars = sorted({ch for key in keys for text in pools[key] for ch in text})
        missing = [ch for ch in chars if font.lookup(ord(ch)) == 0]
        status = "OK" if not missing else f"缺 {len(missing)} 字"
        print(f"\n{filename}: 覆盖检查 {len(chars)} 个不同字符 -> {status}")
        if missing:
            bad = True
            print("  缺字: " + " ".join(f"'{c}'(U+{ord(c):04X})" for c in missing))
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
