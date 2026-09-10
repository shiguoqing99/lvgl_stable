#!/usr/bin/env python3
"""把黑底大图标抠成透明背景并缩放成 LVGL 可用的素材。

用法
----
    python tools/prepare_icon.py <input.png> [size]

行为
----
1. 原图备份为 <名字>_original.png（已存在则跳过，不覆盖）
2. 抠图 + 缩放后**覆盖回原路径**，作为 png_to_lvgl.py 的输入：
       python tools/png_to_lvgl.py main/assert/sun.png main/sun_image.c sun_image
3. 生成 4 倍最近邻放大 + 棋盘格预览 tools/_icon_preview.png，肉眼验收抠图质量

原理
----
黑底图本质上就是「颜色 x alpha」的预乘结果：背景黑 (0,0,0) 相当于 alpha=0。
直接取 alpha = max(R,G,B) 就能得到平滑的透明通道（抗锯齿边缘自动半透明），
不需要颜色阈值，也不会出锯齿。

缩放必须在预乘空间做（LANCZOS 直接插值非预乘 RGBA 会在边缘混入黑色），
最后再反预乘还原成 LVGL 需要的非预乘颜色，否则半透明边缘会偏暗。
"""
import sys
from pathlib import Path

from PIL import Image


def matte_and_resize(im, size):
    r, g, b, _ = im.split()
    alpha = Image.merge("RGB", (r, g, b)).convert("L")
    premult = Image.merge("RGBA", (r, g, b, alpha))
    small = premult.resize((size, size), Image.LANCZOS)

    px = small.load()
    for y in range(size):
        for x in range(size):
            rr, gg, bb, aa = px[x, y]
            if aa == 0:
                px[x, y] = (0, 0, 0, 0)
            elif aa < 255:
                px[x, y] = (
                    min(255, rr * 255 // aa),
                    min(255, gg * 255 // aa),
                    min(255, bb * 255 // aa),
                    aa,
                )
    return small


def main():
    if len(sys.argv) not in (2, 3):
        raise SystemExit(__doc__)
    src = Path(sys.argv[1])
    size = int(sys.argv[2]) if len(sys.argv) == 3 else 64

    im = Image.open(src).convert("RGBA")
    backup = src.with_name(src.stem + "_original.png")
    if not backup.exists():
        backup.write_bytes(src.read_bytes())
        print(f"原图已备份: {backup} ({backup.stat().st_size} 字节)")

    out = matte_and_resize(im, size)
    out.save(src, optimize=True)
    print(f"已输出: {src} ({src.stat().st_size} 字节, {size}x{size}, RGBA)")
    print("下一步: python tools/png_to_lvgl.py", src, "<输出.c> <符号名>")

    big = out.resize((size * 4, size * 4), Image.NEAREST)
    board = Image.new("RGB", big.size, (255, 255, 255))
    p = board.load()
    for y in range(big.size[1]):
        for x in range(big.size[0]):
            if (x // 16 + y // 16) % 2:
                p[x, y] = (185, 185, 185)
    board.paste(big, (0, 0), big)
    board.save("tools/_icon_preview.png")
    print("预览: tools/_icon_preview.png")


if __name__ == "__main__":
    main()
