# ChronoTrace development guide

This document is the public source-build and verified-device maintenance guide.
The shorter product README remains focused on everyday installation and use.

## Build

ChronoTrace requires ESP-IDF 5.5.x:

```sh
. /path/to/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
```

The normal build runs `tools/check_ui_font_coverage.py`. Regenerate all affected
Source Han Sans subsets whenever a user-visible string changes.

## Verified single-factory layout

| Region | Offset | Size |
| --- | ---: | ---: |
| NVS | `0x9000` | `0x5000` |
| factory app | `0x10000` | 15 MB |

Never assume this layout from the repository alone. Identify the physical device,
read its live partition table, and back up NVS before writing.

For an already verified ChronoTrace StopWatch, the normal app-only update is
`build/stopwatch_chronotrace.bin` at `0x10000`. Do not rewrite the bootloader or
partition table for an ordinary application change. Independently run
`verify_flash`, then inspect the 115200-baud startup log.

## Repository checks

```sh
python tools/check_ui_font_coverage.py
idf.py build
git diff --check
```

Compilation is not physical acceptance. Display, touch, buttons, IMU orientation,
RTC, Wi-Fi/BLE coexistence, microphone response, speaker behavior, and long-running
animations still require real-device validation after relevant changes.

Before any public push or release, review secrets, local paths, Git identity,
image metadata, build output, third-party notices, and repository visibility.
Run a full-history secret scan and a staged-tree scan, then verify that GitHub
Secret Scanning and Push Protection are enabled before pushing.
