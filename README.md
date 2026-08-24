# 时迹 ChronoTrace

An independently named, standalone single-firmware adaptation of
[V4C38/esp32-fluidbox](https://github.com/V4C38/esp32-fluidbox) for the M5Stack
StopWatch (ESP32-S3R8, 466 x 466 CO5300 AMOLED, BMI270).

## Design

- One `factory` application partition; there are no OTA/application slots.
- The original two-core FluidBox solver and software renderer are retained.
- The display is sent in two alternating 16-row internal-SRAM DMA bands.
- A 466 x 466 circular simulation volume matches the round StopWatch display.
- BMI270 supplies gravity, shake, and rotational forces.
- Eight coordinated visual/audio themes: deep sea, cyber, lava, aurora, mercury,
  rainbow prism, gold, and diamond. Themes affect particles only; the AMOLED background stays
  true black without gradients or reactive color glow.
- ES8311/I2S procedural ambience follows fluid speed without storing large WAV files.
- Particle motion produces a soft band-limited "sha-sha" surf texture. Its
  overall volume and high-frequency foam follow mean/peak particle speed
  directly. A wide-range non-periodic envelope adds clearly audible crests and
  retreats even during sustained motion, while particle speed remains the
  overall loudness ceiling; it fades as the liquid settles.
- CST820 touch gestures provide particle-clock and visual effects that do not
  duplicate the two physical buttons.
- A short press on the yellow A button opens a borderless handwriting editor.
  It records the actual 64 x 64 stroke shape rather than using OCR, so Chinese,
  English, numbers, and simple symbols all work. One to six glyphs can be saved
  in NVS and are then assembled by the liquid particles in a 2.8-second loop.
  A topology-preserving centreline pass keeps closed counters legible in thick
  handwritten glyphs such as `8`, while deterministic targets and fixed ink
  luminance prevent a held glyph from sparkling or periodically rebuilding.
- A screen tap reads the RX8130CE RTC and briefly assembles real liquid
  particles into an orientation-aware `HH:MM` clock. Gravity keeps it upright
  through portrait, landscape, and upside-down holds; the gyro also tracks
  in-plane rotation while the device lies level. When level, unused particles
  form a drifting halo; when tilted, they settle under gravity.
- Centered Chinese operation messages use a 24 px, 4-bit antialiased subset of
  Source Han Sans CN Medium. They are drawn directly over the scene without a
  panel, border, or decorative glow.
- External-microphone mode separates bass, mids, and treble: bass pushes the
  pool, mids brighten the liquid, and treble lifts highlights.
- The seventh visual theme, `黄金`, grades motion from deep bronze
  through warm gold to champagne highlights on the unchanged true-black AMOLED
  background.
- The eighth theme, `钻石`, uses graphite-blue bodies, icy faces, and stable
  speed-indexed cyan/violet facet highlights without random twinkling.
- Screen power/reset is controlled through the StopWatch M5IOE1 expander.

## Controls

| Input | Action |
| --- | --- |
| A short | Open handwriting editor; press again to cancel |
| A double | Open/close the circular countdown selector; drag anywhere on the ring for 1–60 minutes, then tap the central start button |
| A long | Cycle 650 / 900 / 1000 particle density |
| B short | Next coordinated visual/audio theme |
| B long | Toggle external-music reactive mode |
| A+B long | Open/close Settings |
| Screen tap | Assemble the current `HH:MM` from particles |
| Screen long press | Toggle an always-on particle clock; long press again resets |
| Screen double tap | Burst particles outward from the center |
| Swipe left/right | Blow a particle gust left/right |
| Swipe up/down | Increase/decrease speaker volume |

Settings provides Bluetooth, Chinese/English, and continuously draggable
speaker-volume and AMOLED-brightness sliders. Selecting English localizes the existing theme,
volume, density, handwriting, countdown, Bluetooth, and operation UI rather
than changing only the onboarding pages.

When Bluetooth is enabled, the connectable `ChronoTrace` peripheral accepts
phone time packets through its custom GATT service. A received
time packet immediately calibrates the RX8130CE; later packets provide periodic
correction. Bluetooth does not accept or transmit handwriting/text content.
See `docs/BLE_PROTOCOL.md`.

The countdown menu places four large choices around the circular display.
During a countdown, particles form `MM:SS` while the unused outer halo steadily
shrinks with remaining time. Double-tap cancels into natural fluid motion. At
zero, the formation naturally disperses and a three-note healing chime plays.
Screen long-press remains exclusively assigned to the held particle clock.

Inside the handwriting editor, write inside the enlarged subtle rounded guide
and use `删除`, `下一个`, or `确定` along the curved circular safe area at the
bottom. The side labels follow the screen-edge tangent. The top
counter shows `手写 n/6`.

Eight compact circular colour swatches sit outside the writing field, four on
each side of the round display and vertically centred around the field. The
bright outer ring marks the current selection. Colour
is stored independently for every glyph, so a multi-glyph loop can change
particle colour from one handwritten character to the next. Existing saved
glyphs without colour metadata safely fall back to cyan. Completing after any
one to six glyphs starts particle playback immediately. While handwritten

particles are playing, double-tap the screen to dissolve them into ordinary
liquid. Double-tap again to pull the same particles back into the saved glyph
loop. Both directions are natural and show no text prompt. Before handwriting
has been dissolved, the ordinary-liquid double tap remains the center burst.

The writing field includes a low-contrast dashed centre cross like a Chinese
practice grid. Input uses a fine single-cell stroke. Its enlarged editor
preview uses colour-aware bilinear antialiasing, so diagonal strokes remain
smooth without altering the bitmap used by particle formation. During playback,
handwritten glyphs use exactly the same gravity/gyro pose path and initial
orientation calibration as the particle clock. Handwritten stroke targets stay
fixed; only particles assigned to the flat-only outer halo receive slow orbital
motion.

The PMIC power key remains dedicated to power management. In external-music
mode, continuous speaker ambience is suppressed so the microphone does not
feed the device's own sound back into the visual response. Short UI sounds are
ignored by the analyser for a brief guard interval.

## Build

Requires ESP-IDF 5.5.x. The first build downloads the managed CO5300 and I2C
components; BMI270 and M5IOE1 are vendored under `components/` for reproducible
StopWatch hardware support. Audio uses Espressif's managed `esp_codec_dev`.

```sh
. /path/to/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
```

The current ESP-IDF 5.5.3 build completes successfully. Its application image
leaves 95% of the 15 MB factory partition free. Historical candidate artifacts
remain under `release/`. The installed ChronoTrace build
includes the upright-clock corrections, properly configures the AW8737A
speaker amplifier, and uses softly breathing surf plus pentatonic water-drop
feedback. Flash verification and physical boot initialization passed.

Flashing is deliberately a separate step and should only be done after checking
the connected device identity and its live flash layout.

## Current verification boundary

The source is configured for the official StopWatch pin map. A successful host
build proves compilation only. The following still require a physical device:

- AMOLED initialization and 80 MHz QSPI signal integrity
- BMI270 axis/sign calibration in all orientations
- RX8130CE time validity and particle-clock pose transitions
- rendered circular edge and final frame/physics rates
- two-button short/long/chord timing and PMIC power-key coexistence
- CST820 coordinate orientation and all circular-screen gestures
- handwriting legibility, bottom action hit areas, and all six playback slots
- ES8311 speaker volume, microphone sensitivity, feedback suppression, and
  frequency-band thresholds with real external music

## Licensing

The FluidBox-derived solver and renderer are used under the upstream MIT license
in `LICENSE.FluidBox`. The generated Source Han Sans subset is covered by the
SIL Open Font License in `LICENSE.SourceHanSans`. Vendored components retain
their own license files.
