<div align="center">
  <img src="assets/design/chronotrace-logo-master.png" alt="ChronoTrace logo" width="120">
  <h1>ChronoTrace</h1>
  <p><strong>Time, gravity, and sound become a pool of living particles.</strong></p>
  <p>
    A standalone particle-clock firmware for M5Stack StopWatch,<br>
    with fluid simulation, motion sensing, timers, visual forms, and music response.
  </p>
  <p>
    <a href="#overview">Overview</a> ·
    <a href="#current-firmware">v1.0.0</a> ·
    <a href="#build--flash">Build &amp; flash</a> ·
    <a href="#controls">Controls</a> ·
    <a href="#troubleshooting">Troubleshooting</a> ·
    <a href="#privacy">Privacy</a> ·
    <a href="README.zh-CN.md">简体中文</a>
  </p>
  <p>
    <img alt="Hardware: M5Stack StopWatch" src="https://img.shields.io/badge/hardware-M5Stack%20StopWatch-EA1D2C">
    <img alt="ESP-IDF: 5.5" src="https://img.shields.io/badge/ESP--IDF-5.5-E7352C">
    <img alt="Display: 466 x 466 AMOLED" src="https://img.shields.io/badge/display-466%20%C3%97%20466%20AMOLED-111111">
    <img alt="Version: 1.0.0" src="https://img.shields.io/badge/version-1.0.0-F3A712">
    <img alt="Shapes: 40" src="https://img.shields.io/badge/shapes-40-22A6B3">
    <img alt="Animations: 9" src="https://img.shields.io/badge/animations-9-7C5CFC">
  </p>
</div>

## Current firmware

Version `1.0.0` is the current source firmware. It runs as one standalone
factory application on M5Stack StopWatch and keeps settings, Wi-Fi credentials,
and player drawings in the device's NVS partition.

The current build includes the complete particle clock and countdown experience,
a 40-shape library, nine particle animations, up to 12 saved player drawings,
music-driven particle jumps, on-device Wi-Fi setup, cached weather, Bluetooth
time calibration, bilingual settings, and a branded About screen.

No public release binary is currently advertised from this README. Developers
should build from source and verify the connected hardware and live partition
table before writing any image.

## What's new in v1.0.0

- **Expanded visual library:** 40 built-in forms, random recoloring, ordered
  multi-shape playback, and a circular picker optimized for the round display.
- **Nine particle animations:** four directional arrows, heartbeat, DNA helix,
  firework, asynchronous particle rain, and an `I LOVE YOU` marquee.
- **Owned particle cohorts:** letters and animated structures keep their own
  reserved particles instead of borrowing from one another while moving.
- **Reactive particle motion:** bass, mids, and treble now drive actual pool
  lift, side waves, sparks, and surface jumps rather than color alone.
- **Player drawing library:** save, recolor, play, and delete up to 12 custom
  forms from the on-device editor.
- **Connection and device settings:** on-watch Wi-Fi scanning and password input,
  Bluetooth time calibration, weather refresh, sound, brightness, haptics,
  music reactivity, operation guide, and About page.
- **Refined branding:** the ChronoTrace logo is used at boot and in About,
  alongside developer, version, and copyright information.

### Safe update path

| Image | Flash offset | Use it when | What it preserves |
| --- | ---: | --- | --- |
| `build/stopwatch_chronotrace.bin` | `0x10000` | Updating an already verified ChronoTrace StopWatch | Preserves NVS, bootloader, and partition table |

> [!WARNING]
> ChronoTrace uses one `factory` application partition; it is not a dual-app OTA
> layout. Identify the physical device and read its live partition table before
> writing. Back up NVS at `0x9000` with size `0x5000`, then update only the app
> image at `0x10000`. Never copy offsets from another ESP32-S3 project.

## Overview

ChronoTrace is a standalone M5Stack StopWatch firmware derived from the solver
and software renderer in
[V4C38/esp32-fluidbox](https://github.com/V4C38/esp32-fluidbox). The display is
not a prerecorded animation: every visible particle responds to gravity,
rotation, touch, the selected formation, and—when enabled—external music.

| | Capability | What it does |
| --- | --- | --- |
| **01** | Live particle fluid | Runs dual-core physics and software rendering inside the round 466 x 466 AMOLED volume. |
| **02** | Time and orientation | Uses BMI270 motion data and the RX8130CE RTC for upright digital and analogue particle clocks. |
| **03** | Particle countdown | Provides a circular 1–60 minute selector, `MM:SS` formation, pause/resume, and completion feedback. |
| **04** | Shapes and animations | Offers 40 built-in shapes, nine animations, random switching, and ordered loop playback. |
| **05** | Player drawings | Stores up to 12 custom shapes with per-drawing color, playback, and deletion. |
| **06** | Sound and music response | Generates procedural device audio and maps external bass, mids, and treble to physical particle motion. |
| **07** | Connection and information | Provides on-device Wi-Fi setup, cached weather, BLE time calibration, and Chinese/English UI. |

## Device experience

### Particle time

- Tap the default scene to assemble the current `HH:MM` time.
- Long-press to hold the clock. Double-tap a held clock to switch between
  digital and analogue styles.
- Countdown digit changes conserve shared particles: only a deficit is pulled
  from the surrounding fluid, while surplus particles fall naturally under the
  current gravity.
- During charging, pool particles periodically jump up to form a short energy
  line below the battery, dissolve, and assemble again.

### Shape library

The built-in library contains 40 forms. Built-in forms receive a fresh bright
particle color when they appear; custom drawings retain the player's selected
color.

`Heart` `Star` `Pentagram` `Circle` `Square` `Triangle` `Diamond` `Hexagon`
`Octagon` `Moon` `Infinity` `Cross` `Wave` `Flower 5` `Flower 6` `Flower 8`
`Butterfly` `Clover` `Maple Leaf` `Drop` `Lightning` `Cloud` `Sun` `Rainbow`
`Fish` `Bird` `Cat` `Rabbit` `Smile` `Music` `Crown` `Rocket`
`Planet` `Snowflake` `Umbrella` `House` `Tree` `Mountains` `Apple` `Hourglass`

Swipe right from the default scene to enter shape mode. Double-tap for a random
shape or long-press to open the picker. Select several items in order to create
a loop, or open the drawing editor to add player-made forms.

### Animation library

Swipe down in the shape picker to open animations; swipe up to return to shapes.

| Animation | Particle behavior |
| --- | --- |
| Arrow up / down / left / right | Builds progressively, crosses nearly the full screen diameter, then naturally falls apart at the far edge. |
| Heartbeat | Expands and contracts with a fast two-stage heartbeat rhythm. |
| DNA helix | Moves a minimal double-chain structure with a continuous phase shift. |
| Firework | Launches a preassembled particle ball with a short tail before it bursts. |
| Particle rain | Runs multiple vertical streams with independent position, phase, and speed. |
| `I LOVE YOU` | Raises and scrolls independently owned letter cohorts without borrowing particles between letters. |

Recognizable formations do not morph directly into one another. ChronoTrace
releases the old form into the real fluid first, leaves it visibly loose, and
then attracts particles into the next form.

### Music reactivity

Music mode changes particle motion, not only color:

- bass pulses lift surface particles out of the pool;
- mids create alternating side waves;
- treble produces sparse sparks and small jumps;
- sustained strong beats add larger layered ejections while preserving fluid
  gravity and collision behavior.

Continuous procedural ambience is suppressed in reactive mode so the microphone
does not repeatedly analyze the device's own speaker. Short UI sounds also have
a brief analyzer guard interval.

### What the main screens show

| Screen | Purpose |
| --- | --- |
| Default fluid | Tilt-aware particle pool, themes, procedural sound, and music response. |
| Particle clock | Timed or held digital/analogue time, always upright for the current pose. |
| Countdown | Circular minute selector, particle digits, pause/resume, and cancel. |
| Shape picker | 40 built-in forms, player drawings, ordered selection, play, exit, and delete. |
| Animation picker | Nine particle animations selected from the same circular interface. |
| Drawing editor | Round-screen practice grid, eight colors, return, delete, next, and confirm. |
| Weather | Cached city, weather form, temperature, and daily range. |
| Settings | Connection page, device page, operation guide, and branded About page. |

## Hardware support

| Hardware | Current support |
| --- | --- |
| **M5Stack StopWatch** | Primary and physically tested target: ESP32-S3R8, 466 x 466 CO5300 AMOLED, BMI270, CST820, RX8130CE, ES8311, and AW8737A. |
| **Other ESP32-S3 boards** | Not supported. Display, touch, RTC, audio, PMIC, and flash layout are StopWatch-specific. |

## Before you start

- [ ] M5Stack StopWatch and a USB data cable.
- [ ] ESP-IDF 5.5.x with ESP32-S3 support.
- [ ] The serial port of the connected device, shown below as `<PORT>`.
- [ ] Permission to preserve or back up the device's current NVS data.

<p align="center"><strong>Inspect device → Back up NVS → Build → Flash app only → Verify → Monitor</strong></p>

## Build & flash

### Build from source

Load ESP-IDF from the repository root and build:

```sh
. /path/to/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
```

The application image is produced at:

```text
build/stopwatch_chronotrace.bin
```

The normal build also runs `tools/check_ui_font_coverage.py`. Any changed
Chinese or English UI string requires regenerated 20 px and 24 px Source Han
Sans CN Medium subsets; do not bypass the coverage check.

### Verify the physical device

Read the chip identity, flash size, and live partition table before writing.
The verified ChronoTrace layout is:

| Region | Offset | Size |
| --- | ---: | ---: |
| NVS | `0x9000` | `0x5000` |
| factory app | `0x10000` | 15 MB |

Back up NVS before an ordinary firmware update:

```sh
esptool.py --chip esp32s3 --port <PORT> \
  read_flash 0x9000 0x5000 backups/nvs-before-update.bin
```

### Flash the application only

```sh
esptool.py --chip esp32s3 --port <PORT> \
  write_flash 0x10000 build/stopwatch_chronotrace.bin
```

Then independently verify the same image and inspect the 115200-baud startup
log. A successful write alone is not a complete hardware acceptance.

```sh
esptool.py --chip esp32s3 --port <PORT> \
  verify_flash 0x10000 build/stopwatch_chronotrace.bin
python -m serial.tools.miniterm <PORT> 115200
```

## Controls

### Physical buttons

| Input | Action |
| --- | --- |
| A short | Open the player drawing editor; press again while editing to cancel. |
| A double | Open or close the 1–60 minute countdown selector. |
| A long | Cycle 650 / 900 / 1000 particle density. |
| B short | Switch to the next particle theme. |
| B double | Show battery level and charging state. |
| B long | Toggle external-music reactivity. |
| A + B long | Open or close Settings. |

### Default-scene touch gestures

| Gesture | Action |
| --- | --- |
| Tap | Assemble the current particle time. |
| Long press | Hold or release the always-on particle clock. |
| Double tap | Switch digital/analogue clock while a clock is active; otherwise show a random form. |
| Swipe left | Open the cached particle-weather view. |
| Swipe right | Enter shape mode. |
| Swipe up / down | Raise / lower speaker volume. |

## Configuration

Settings has two pages, switched by swiping or the A/B buttons:

- **Connections:** Wi-Fi, on-device network search and password entry,
  Bluetooth/time calibration, automatic city weather, and Chinese/English.
- **Device:** sound, brightness, haptics, music reactivity, operation guide,
  and About.

The BLE peripheral is named `ChronoTrace`. Its custom GATT service accepts phone
time packets to calibrate the RX8130CE. It does not receive or transmit player
drawings or text. See [`docs/BLE_PROTOCOL.md`](docs/BLE_PROTOCOL.md).

## Troubleshooting

### `command not found: idf.py`

Load the ESP-IDF environment in the current shell:

```sh
. /path/to/esp-idf/export.sh
```

### The device cannot enter flashing mode

Confirm that no serial monitor owns `<PORT>`, use a real USB data cable, and
retry with the matching `/dev/tty.*` port if `/dev/cu.*` does not complete the
reset handshake. Do not erase flash as a first troubleshooting step.

### The screen stays frozen after a flash

First verify the app image independently, then hard-reset and read the serial
log. Rewriting the bootloader or partition table is not an appropriate routine
fix for an app-only update.

### Wi-Fi shows no networks

Wait for the asynchronous search to finish, confirm 2.4 GHz Wi-Fi is available,
and reopen the on-device network selector. ESP32-S3 does not join 5 GHz-only
networks.

### Chinese or English text shows a replacement box

Regenerate both Source Han Sans subsets and rebuild. Compilation should fail if
a known runtime string is missing from either generated font resource.

### Music changes color but particles do not jump

Confirm music reactivity is enabled, device ambience is suppressed, and the
external source is loud enough at the microphone. Bass energy drives the
largest vertical pool motion.

## Privacy

- Firmware contains no developer Wi-Fi password, API key, fixed token, or
  hard-coded personal device identifier.
- User Wi-Fi credentials are entered on the device and stored locally in NVS.
- When Wi-Fi is enabled, weather uses network location and Open-Meteo data;
  disabling Wi-Fi stops those network requests.
- BLE is used for time calibration and does not upload drawings, playlists, or
  audio.

Before publishing source, binaries, logs, or screenshots, review local paths,
Git author identity, image metadata, generated configuration, and build output.

## Project layout

```text
StopWatch-ChronoTrace/
  assets/design/          Logo and brand source artwork
  components/             Vendored hardware components
  docs/                   Protocol and supporting documentation
  main/                   Physics, rendering, interaction, audio, and network code
  tools/                  Logo, icon, font generation, and coverage checks
  CMakeLists.txt
  README.md
  README.zh-CN.md
```

## Checks

Run from the repository root:

```sh
python tools/check_ui_font_coverage.py
idf.py build
git diff --check
```

The current ESP-IDF 5.5.3 build produces an application image of about 1.66 MiB
inside the 15 MB factory partition. On the verified device, continuous runtime
logs are approximately 56 FPS without a startup reboot loop.

## Current limits

- M5Stack StopWatch is the only supported hardware target.
- No general-user factory image or public Release package is currently promised
  by this README; the documented route is a verified source build and app-only
  update.
- Network and microphone behavior still depends on router, room acoustics, and
  real-device conditions.
- Long-duration animation loops, Wi-Fi/BLE coexistence, and every physical
  orientation should be rechecked after large simulator or memory changes.

## Contributing & security

Keep hardware-specific changes scoped to StopWatch and preserve the live NVS
layout. Do not commit credentials, device backups, serial logs with private data,
or generated local build directories. Before a GitHub push, run a local secret
and privacy review and confirm repository Secret Scanning and Push Protection.

For a security-sensitive report, contact the repository owner privately instead
of publishing credentials or device data in an issue.

## Copyright, assets & licenses

ChronoTrace is developed by **Xiao Ao**. Copyright © 2026 Xiao Ao.

- FluidBox-derived solver and renderer code is covered by the MIT terms in
  [`LICENSE.FluidBox`](LICENSE.FluidBox).
- Generated Source Han Sans subsets are covered by the SIL Open Font License in
  [`LICENSE.SourceHanSans`](LICENSE.SourceHanSans).
- Source Han Serif resources are covered by the SIL Open Font License in
  [`LICENSE.SourceHanSerif`](LICENSE.SourceHanSerif).
- Vendored components retain their own license files and notices.
