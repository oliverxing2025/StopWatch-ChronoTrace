# UI font generation

The firmware embeds only the glyphs used by quoted UI strings under `main/`.
It does not embed the complete desktop font.

Current source font: Adobe Source Han Sans CN Medium, official subset OTF:

`https://raw.githubusercontent.com/adobe-fonts/source-han-sans/release/SubsetOTF/CN/SourceHanSansCN-Medium.otf`

Verified source SHA-256:
`a94e558a2fe972bee4f46bce0843abff37063fd68c33f1e7d9058f6f09432b01`.
The license is retained at `LICENSE.SourceHanSans`.

Regenerate after adding or changing a Chinese UI string:

```sh
python3 tools/generate_ui_font.py \
  --font /path/to/SourceHanSansCN-Medium.otf --size 24
```

The script requires Pillow. The complete font is intentionally kept outside
the firmware tree; only the generated C subset is checked in.

The generator rescans the source, validates every required codepoint against
the font cmap, and rewrites the generated C resource plus
`tools/ui_font_charset.txt`. The complete OTF remains a build-time input and is
not linked into the firmware.
