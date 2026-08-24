#!/usr/bin/env python3
"""Convert the ChronoTrace master logo to a compact RGB565 firmware asset."""

from pathlib import Path

from PIL import Image


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "assets" / "design" / "chronotrace-logo-master.png"
HEADER = ROOT / "main" / "boot_logo.h"
SOURCE_C = ROOT / "main" / "boot_logo.c"
SIZE = 248


def main() -> None:
    image = Image.open(SOURCE).convert("RGBA")
    alpha = image.getchannel("A")
    # Image generators sometimes leave isolated, nearly transparent pixels at
    # the canvas edge. Ignore them so the real circular mark remains centred
    # and uses the intended amount of the watch face.
    visible_alpha = alpha.point(lambda value: 255 if value > 8 else 0)
    bbox = visible_alpha.getbbox()
    if bbox is None:
        raise SystemExit("logo contains no visible pixels")

    # Preserve the complete glass glow while removing the empty source canvas.
    image.putalpha(Image.eval(alpha, lambda value: value if value > 8 else 0))
    image = image.crop(bbox)
    side = max(image.size)
    square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    square.alpha_composite(image, ((side - image.width) // 2,
                                   (side - image.height) // 2))
    square = square.resize((SIZE, SIZE), Image.Resampling.LANCZOS)

    # The splash always sits on AMOLED black, so pre-compositing removes the
    # need for a second alpha plane while retaining the translucent glass rim.
    black = Image.new("RGBA", square.size, (0, 0, 0, 255))
    black.alpha_composite(square)
    rgb = black.convert("RGB")
    pixels = []
    for red, green, blue in rgb.get_flattened_data():
        pixels.append(((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3))

    HEADER.write_text(
        "#pragma once\n\n"
        "#include <stdint.h>\n\n"
        f"#define BOOT_LOGO_WIDTH {SIZE}\n"
        f"#define BOOT_LOGO_HEIGHT {SIZE}\n\n"
        "extern const uint16_t g_boot_logo_rgb565[BOOT_LOGO_WIDTH * BOOT_LOGO_HEIGHT];\n",
        encoding="utf-8",
    )

    lines = [
        '#include "boot_logo.h"',
        "",
        "const uint16_t g_boot_logo_rgb565[BOOT_LOGO_WIDTH * BOOT_LOGO_HEIGHT] = {",
    ]
    for offset in range(0, len(pixels), 12):
        row = ", ".join(f"0x{value:04X}" for value in pixels[offset:offset + 12])
        lines.append(f"    {row},")
    lines.extend(["};", ""])
    SOURCE_C.write_text("\n".join(lines), encoding="utf-8")
    print(f"generated {SIZE}x{SIZE} RGB565 logo ({len(pixels) * 2} bytes)")


if __name__ == "__main__":
    main()
