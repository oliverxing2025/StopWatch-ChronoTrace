#!/usr/bin/env python3
"""Convert the transparent Image2 timer icon into a compact RGBA asset."""

from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main/assets/countdown-timer-liquid-glass-image2.png"
HEADER = ROOT / "main/countdown_timer_icon.h"
WIDTH = 38
HEIGHT = 38
CONTENT_SIZE = 34


def main() -> None:
    source = Image.open(SOURCE).convert("RGBA")
    alpha_bbox = source.getchannel("A").getbbox()
    if alpha_bbox is None:
        raise RuntimeError("Image2 timer icon has no visible pixels")

    cropped = source.crop(alpha_bbox)
    cropped.thumbnail((CONTENT_SIZE, CONTENT_SIZE), Image.Resampling.LANCZOS)
    icon = Image.new("RGBA", (WIDTH, HEIGHT), (0, 0, 0, 0))
    offset = ((WIDTH - cropped.width) // 2, (HEIGHT - cropped.height) // 2)
    icon.alpha_composite(cropped, offset)
    values = list(icon.getdata())

    lines = [
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define COUNTDOWN_TIMER_ICON_W {WIDTH}",
        f"#define COUNTDOWN_TIMER_ICON_H {HEIGHT}",
        "",
        "// Image2 liquid-glass countdown icon with per-pixel alpha.",
        "static const uint8_t countdown_timer_icon_rgba[] = {",
    ]
    for row in range(HEIGHT):
        chunk = values[row * WIDTH : (row + 1) * WIDTH]
        encoded = []
        for red, green, blue, alpha in chunk:
            encoded.extend((red, green, blue, alpha))
        lines.append("    " + ", ".join(str(value) for value in encoded) + ",")
    lines.extend(["};", ""])
    HEADER.write_text("\n".join(lines), encoding="utf-8")
    print(f"Wrote {HEADER}: {WIDTH}x{HEIGHT}, {len(values) * 4} bytes")


if __name__ == "__main__":
    main()
