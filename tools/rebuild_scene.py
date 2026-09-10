#!/usr/bin/env python3
"""用新的 main/assert/characters.png 重建 dark/light 场景底图并生成 LVGL C 数组。

流程
----
1. 原图 (任意尺寸, 透明背景) -> 预乘空间 LANCZOS 缩放到 240x320 -> 反预乘还原
2. 与 main/assert/dark.png / light.png 做 alpha_composite（人物在上, 天空在下）
3. 输出不透明 RGB -> main/dark_scene_image.c / main/light_scene_image.c
4. 额外输出 3 倍放大预览 tools/_scene_preview.png 供肉眼验收

用法
----
    python tools/rebuild_scene.py
"""
import sys
from pathlib import Path

from PIL import Image, ImageChops

ROOT = Path(__file__).resolve().parent.parent
ASSERT = ROOT / "main" / "assert"
W, H = 240, 320


def premult_resize(im: Image.Image, size):
    """在预乘空间缩放, 避免透明像素的 (0,0,0) 被插值进边缘造成黑边。

    两种素材要分开处理, 否则会整体变浅:
      1. 正常透明 PNG（自带 alpha 通道, 不透明处 alpha=255）
         -> 必须用**原始 alpha** 预乘。若误用亮度当 alpha, 深色区域
            (黑头发/深色衣服) 的 alpha 会被算成几十, 合成后严重褪色。
      2. 黑底图（alpha 全 255, 背景纯黑）
         -> 本质是「颜色 x alpha」的预乘结果, alpha 由亮度推导。
    """
    r, g, b, a = im.split()
    if a.getextrema()[0] < 255:
        alpha = a                                   # 情况 1: 真实 alpha
        premult = Image.merge("RGBA", (
            ImageChops.multiply(r, alpha),
            ImageChops.multiply(g, alpha),
            ImageChops.multiply(b, alpha),
            alpha,
        ))
    else:
        alpha = Image.merge("RGB", (r, g, b)).convert("L")   # 情况 2: 黑底图
        premult = Image.merge("RGBA", (r, g, b, alpha))
    small = premult.resize(size, Image.LANCZOS)

    px = small.load()
    w, h = size
    for y in range(h):
        for x in range(w):
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


def to_lvgl_c(img: Image.Image, symbol: str) -> str:
    """RGB565 + 每像素 2 字节, LV_IMG_CF_TRUE_COLOR(不透明)。"""
    rgb = img.convert("RGB")
    values = []
    for red, green, blue in rgb.getdata():
        color = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)
        values.extend((color >> 8, color & 0xFF))

    lines = []
    for i in range(0, len(values), 24):
        lines.append("    " + ", ".join(f"0x{v:02X}" for v in values[i:i + 24]) + ",")

    return (
        '#include "lvgl.h"\n\n'
        f"static const uint8_t {symbol}_data[] = {{\n"
        + "\n".join(lines)
        + "\n};\n\n"
        f"const lv_img_dsc_t {symbol} = {{\n"
        "    .header.cf = LV_IMG_CF_TRUE_COLOR,\n"
        f"    .header.w = {rgb.width},\n"
        f"    .header.h = {rgb.height},\n"
        f"    .data_size = sizeof({symbol}_data),\n"
        f"    .data = {symbol}_data,\n"
        "};\n"
    )


def main():
    src = ASSERT / "characters.png"
    if not src.exists():
        raise SystemExit(f"缺少素材: {src}")

    raw = Image.open(src).convert("RGBA")
    print(f"原图: {raw.width}x{raw.height} RGBA, {src.stat().st_size} 字节")

    # 宽高比不一致时按「覆盖整屏」裁切, 一致时直接缩放
    target_ratio = W / H
    src_ratio = raw.width / raw.height
    if abs(src_ratio - target_ratio) > 1e-3:
        if src_ratio > target_ratio:  # 原图偏宽 -> 裁左右
            new_w = int(raw.height * target_ratio)
            left = (raw.width - new_w) // 2
            raw = raw.crop((left, 0, left + new_w, raw.height))
        else:  # 原图偏高 -> 裁上下
            new_h = int(raw.width / target_ratio)
            top = (raw.height - new_h) // 2
            raw = raw.crop((0, top, raw.width, top + new_h))
        print(f"裁切至: {raw.width}x{raw.height}")

    chars = premult_resize(raw, (W, H))
    print(f"缩放至: {W}x{H}")

    previews = []
    for name, symbol in (("light", "light_scene_image"), ("dark", "dark_scene_image")):
        bg = Image.open(ASSERT / f"{name}.png").convert("RGBA")
        if bg.size != (W, H):
            bg = bg.resize((W, H), Image.LANCZOS)
        scene = Image.alpha_composite(bg, chars).convert("RGB")

        out = ROOT / "main" / f"{name}_scene_image.c"
        out.write_text(to_lvgl_c(scene, symbol), encoding="ascii")
        print(f"已生成: {out} ({out.stat().st_size} 字节)")
        previews.append(scene)

    # 预览: 两张并排 3 倍放大
    scale = 3
    board = Image.new("RGB", (W * scale * 2 + 30, H * scale), (20, 20, 20))
    for i, img in enumerate(previews):
        board.paste(img.resize((W * scale, H * scale), Image.NEAREST), (i * (W * scale + 30), 0))
    pv = ROOT / "tools" / "_scene_preview.png"
    board.save(pv)
    print(f"预览: {pv}  (左=白天 light, 右=夜晚 dark)")


if __name__ == "__main__":
    main()
