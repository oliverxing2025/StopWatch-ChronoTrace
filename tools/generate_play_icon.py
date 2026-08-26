#!/usr/bin/env python3
"""Convert the selected play artwork into a compact RGBA asset."""

from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main/assets/countdown-play-user.png"
HEADER = ROOT / "main/countdown_play_icon.h"
WIDTH = 64
HEIGHT = 64
CONTENT_SIZE = 58


def main() -> None:
    source = Image.open(SOURCE).convert("RGBA")
    alpha_bbox = source.getchannel("A").getbbox()
    if alpha_bbox is None:
        raise RuntimeError("Image2 icon has no visible pixels")

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
        f"#define COUNTDOWN_PLAY_ICON_W {WIDTH}",
        f"#define COUNTDOWN_PLAY_ICON_H {HEIGHT}",
        "",
        "// User-selected liquid-glass play button with per-pixel alpha.",
        "static const uint8_t countdown_play_icon_rgba[] = {",
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
