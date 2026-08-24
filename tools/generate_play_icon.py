#!/usr/bin/env python3
"""Convert the transparent Image2 play icon into a compact RGB565 asset."""

from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main/assets/countdown-play-liquid-glass-image2.png"
HEADER = ROOT / "main/countdown_play_icon.h"
WIDTH = 40
HEIGHT = 40
CONTENT_SIZE = 34
BUTTON_RGB = (17, 30, 41)


def rgb565_swapped(red: int, green: int, blue: int) -> int:
    native = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)
    return ((native >> 8) | ((native & 0xFF) << 8)) & 0xFFFF


def main() -> None:
    source = Image.open(SOURCE).convert("RGBA")
    alpha_bbox = source.getchannel("A").getbbox()
    if alpha_bbox is None:
        raise RuntimeError("Image2 icon has no visible pixels")

    cropped = source.crop(alpha_bbox)
    cropped.thumbnail((CONTENT_SIZE, CONTENT_SIZE), Image.Resampling.LANCZOS)
    foreground = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    offset = ((WIDTH - cropped.width) // 2, (HEIGHT - cropped.height) // 2)
    foreground.alpha_composite(cropped, offset)

    background = Image.new("RGBA", (WIDTH, HEIGHT), BUTTON_RGB + (255,))
    composed = Image.alpha_composite(background, foreground).convert("RGB")
    values = [rgb565_swapped(*pixel) for pixel in composed.getdata()]

    lines = [
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define COUNTDOWN_PLAY_ICON_W {WIDTH}",
        f"#define COUNTDOWN_PLAY_ICON_H {HEIGHT}",
        "",
        "// Image2 liquid-glass play icon, precomposited for the countdown button.",
        "static const uint16_t countdown_play_icon_rgb565[] = {",
    ]
    for row in range(HEIGHT):
        start = row * WIDTH
        chunk = values[start : start + WIDTH]
        lines.append("    " + ", ".join(f"0x{value:04x}" for value in chunk) + ",")
    lines.extend(["};", ""])
    HEADER.write_text("\n".join(lines), encoding="utf-8")
    print(f"Wrote {HEADER}: {WIDTH}x{HEIGHT}, {len(values) * 2} bytes")


if __name__ == "__main__":
    main()
