# ChronoTrace project rules

## UI font coverage

- Any new or changed user-visible text must be treated as a font-resource change, whether the text is Chinese or English.
- Before compiling or flashing, rescan `main/` and regenerate every Source Han Sans subset used by the changed UI role. At minimum, regenerate both 20 px setting labels and 24 px centered messages when shared UI strings change.
- Chinese glyphs must come from the scanned Source Han Sans CN Medium subset. Printable ASCII must remain fully embedded so English letters, spaces, digits, and punctuation cannot fall back to the replacement box.
- Inspect the generated charset and verify every codepoint in the new runtime string is present. A successful compile alone is not sufficient verification.
- Do not flash a build that can show `□` for a known runtime string. The replacement-box glyph is only a last-resort safety fallback for unexpected external data.
- Keep `tools/check_ui_font_coverage.py` enabled in the normal ESP-IDF build. Never bypass or remove this check to make a stale font subset compile.
- After regeneration, compile the complete firmware and visually check the affected Chinese and English screens on the StopWatch AMOLED display.

## Particle formation transitions

- Never morph one recognizable particle formation directly into another. Release the old formation into the real fluid simulation first, leave roughly 600–700 ms for the particles to fall and loosen visibly, and only then attract particles into the new formation.
- Returning from a particle information view should also release its formation naturally. Preserve any persistent mode (for example a held clock) and restore it only after leaving the temporary view.
- For the analogue clock second hand, keep every bead at the same rendered particle radius as the rest of the clock face. Make the second hand visually lighter only by reducing the number of particles assigned to it; never introduce a second-hand-only particle-size style.

## Firmware flashing workflow

- Treat this project as a continuation of the established ChronoTrace device workflow. Flash only after the requested source change has compiled and its relevant checks have passed.
- Before writing, confirm `/dev/cu.usbmodem2101` is the expected ESP32-S3 with 16 MB flash, read the live partition table, and confirm the factory application still begins at `0x10000`.
- Back up the live NVS range at `0x9000` with size `0x5000` before a write whenever the current task does not already have a verified backup. Never erase or overwrite NVS for an application-only update.
- For ordinary firmware changes, write only `build/stopwatch_chronotrace.bin` at `0x10000`. Do not rewrite the bootloader or partition table unless the requested change actually requires them and their live layout has been verified first.
- After writing, run an independent `verify_flash` against the same application image, then read the 115200-baud serial log and confirm normal ChronoTrace startup without a reboot loop. A successful write alone is not completion.
