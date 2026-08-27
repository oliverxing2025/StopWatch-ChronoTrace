<div align="center">
  <img src="assets/design/chronotrace-logo-master.png" alt="ChronoTrace logo" width="120">
  <h1>ChronoTrace</h1>
  <p><strong>When particles begin to flow, time is only one of their forms.</strong></p>
  <p>An independent particle experience for M5Stack StopWatch.<br>The particles respond to gravity, touch, and sound, then assemble into time, shapes, and animations.</p>
  <p>
    <a href="#core-experience">Experience</a> ·
    <a href="#installation">Installation</a> ·
    <a href="#controls">Controls</a> ·
    <a href="#privacy">Privacy</a> ·
    <a href="#license-and-attribution">License</a> ·
    <a href="README.zh-CN.md">简体中文</a>
  </p>
  <p>
    <img alt="Hardware: M5Stack StopWatch" src="https://img.shields.io/badge/hardware-M5Stack%20StopWatch-EA1D2C">
    <img alt="Display: 466 x 466 AMOLED" src="https://img.shields.io/badge/display-466%20%C3%97%20466%20AMOLED-111111">
    <img alt="Version: 1.0.0" src="https://img.shields.io/badge/version-1.0.0-F3A712">
    <img alt="Shapes: 40" src="https://img.shields.io/badge/shapes-40-22A6B3">
    <img alt="Animations: 9" src="https://img.shields.io/badge/animations-9-7C5CFC">
  </p>
</div>

<p align="center">
  <img src="docs/images/chronotrace-hero.jpg" alt="ChronoTrace product concept" width="560">
  <br><sub>Concept visual; the particle simulation is rendered live on the device.</sub>
</p>

## This is not a prerecorded animation

Every visible particle participates in the live simulation. Tilt the device and
the pool follows gravity. Touch the screen and particles gather, dissolve, and
assemble into recognizable forms. In music-reactive mode, bass, mids, and treble
drive different layers of particle motion.

The current product version is `1.0.0`. Source and verified release assets are
published through this repository's GitHub Releases page. Any M5Burner listing
or Share Code is valid only when announced by the developer.

## Core experience

- **Live particle fluid:** motion, touch, collision, and rotation affect a real-time simulation rather than a looping video.
- **Particle time:** tap to assemble the current time, hold it on screen, and switch between digital and analogue styles.
- **Particle countdown:** select 1–60 minutes, pause, resume, and receive completion feedback.
- **Shapes and animations:** 40 built-in forms, nine animations, random switching, and ordered loops.
- **Draw your own forms:** create and save up to 12 colored drawings directly on the round display.
- **Music reactivity:** external sound drives waves, jumps, and sparks instead of changing color alone.
- **Connection and information:** on-device Wi-Fi setup, weather, Bluetooth time calibration, and Chinese/English UI.
- **Sound and touch:** procedural audio, volume, brightness, and haptic settings are controlled on the device.

<p align="center">
  <img src="docs/images/chronotrace-particle-heart.jpg" alt="ChronoTrace particle heart" width="46%">
  <img src="docs/images/chronotrace-particle-clock.jpg" alt="ChronoTrace particle clock" width="46%">
</p>

## Supported hardware

<div align="center">
<table align="center">
  <thead>
    <tr><th align="center">Device</th><th align="center">Support</th></tr>
  </thead>
  <tbody>
    <tr><td align="center"><strong>M5Stack StopWatch</strong></td><td align="center">The only officially supported and physically tested target.</td></tr>
    <tr><td align="center">Other ESP32-S3 devices</td><td align="center">Not supported; display, touch, RTC, audio, PMIC, and flash layouts differ.</td></tr>
  </tbody>
</table>
</div>

Confirm that the connected device is an M5Stack StopWatch. Do not flash this
firmware to a different ESP32-S3 product.

## Installation

1. Install and open M5Burner from an official M5Stack source.
2. Open ChronoTrace through the developer's official product page or Share Code.
3. Confirm the target is **M5Stack StopWatch** and connect it with a USB data cable.
4. Follow M5Burner's download and burn flow. Do not disconnect USB or remove power during the operation.
5. On first boot, follow the on-screen flow to choose language, sound, brightness, and optional connectivity.

> [!WARNING]
> Installing third-party firmware replaces the device's current program and may
> remove or alter data saved by the previous firmware. Make sure you are authorized
> to modify the device and have the official M5Stack recovery path available.

ChronoTrace is not currently a generic dual-slot OTA image. End users should not
guess flash offsets or apply `esptool.py` instructions from another project. Use
the official M5Stack recovery firmware and instructions when recovery is needed.

Developers who want to build from source should follow the
[development guide](docs/DEVELOPMENT.md).

## Controls

For full instructions, see the [ChronoTrace User Guide](docs/USER_GUIDE.md).
An on-device four-page quick guide is also available under
**Settings → Operation Guide**.

### Physical buttons

<div align="center">
<table align="center">
  <thead>
    <tr><th align="center">Input</th><th align="center">Action</th></tr>
  </thead>
  <tbody>
    <tr><td align="center">A short</td><td align="center">Open or leave the drawing editor.</td></tr>
    <tr><td align="center">A double</td><td align="center">Open or close the countdown.</td></tr>
    <tr><td align="center">A long</td><td align="center">Change particle density.</td></tr>
    <tr><td align="center">B short</td><td align="center">Change the particle theme.</td></tr>
    <tr><td align="center">B double</td><td align="center">Show battery and charging status.</td></tr>
    <tr><td align="center">B long</td><td align="center">Toggle music reactivity.</td></tr>
    <tr><td align="center">A + B long</td><td align="center">Open or close Settings.</td></tr>
  </tbody>
</table>
</div>

### Default-scene gestures

<div align="center">
<table align="center">
  <thead>
    <tr><th align="center">Gesture</th><th align="center">Action</th></tr>
  </thead>
  <tbody>
    <tr><td align="center">Tap</td><td align="center">Assemble the current time.</td></tr>
    <tr><td align="center">Long press</td><td align="center">Hold or release the persistent particle clock.</td></tr>
    <tr><td align="center">Double tap</td><td align="center">Switch digital/analogue style while a clock is visible; otherwise show a random form.</td></tr>
    <tr><td align="center">Swipe left</td><td align="center">Open weather.</td></tr>
    <tr><td align="center">Swipe right</td><td align="center">Enter shape mode.</td></tr>
    <tr><td align="center">Swipe up / down</td><td align="center">Raise / lower volume.</td></tr>
  </tbody>
</table>
</div>

## Privacy

- No account is required, and the firmware contains no advertising or behavior-analytics module.
- Wi-Fi credentials are entered by the user and stored locally in the device's NVS.
- The provisioning page intentionally shows the Wi-Fi password as plain text; use it only in a private setting.
- Automatic weather uses the public IP address to request an approximate city and coordinates from `ipwho.is`; manual mode sends the city entered by the user.
- Coordinates are sent to Open-Meteo to obtain current conditions and the day's temperature range.
- Drawings, microphone audio, and playback content are not uploaded. Microphone samples are processed locally for music reactivity.
- Bluetooth is used for time calibration and does not transfer drawings or microphone audio.
- Disabling Wi-Fi stops weather requests. Use the vendor's erase and recovery flow when local data must also be removed.

See the [privacy notice](docs/PRIVACY.md) for details. Third-party services remain
subject to their own privacy policies.

## Current limitations

- Only 2.4 GHz Wi-Fi is supported.
- Automatic city detection is an approximate public-IP lookup, not GPS.
- Weather and music response depend on the network, router, room acoustics, and microphone distance.
- This version does not provide a general-purpose OTA path; follow the official release instructions for updates.
- A general M5Burner image is not a guarantee against copying or reverse engineering.

## Data removal and recovery

- Disable Wi-Fi or Bluetooth from the connection settings.
- Remove saved drawings from the shape picker.
- To remove ChronoTrace completely, restore the device's official firmware through M5Stack's official M5Burner flow.
- Confirm whether that recovery flow erases NVS when local data must also be deleted; rewriting only the app may preserve NVS.
- If previous device data matters, follow the hardware vendor's backup guidance before restoring or replacing firmware.

## Support and security reports

Use the official release page or developer announcement for support. Report
security issues through GitHub's private vulnerability-reporting form; see
[SECURITY.md](SECURITY.md). Do not post Wi-Fi credentials, device backups,
serial numbers, or logs containing personal data.

## License and attribution

ChronoTrace is developed by **Xiao Ao**. Copyright © 2026 Xiao Ao.

- Original ChronoTrace source code and documentation are released under the [MIT License](LICENSE).
- FluidBox, fonts, and hardware components remain under their respective MIT, BSD-3-Clause, Apache-2.0, and SIL OFL terms.
- The ChronoTrace name and logo identify official project releases; the MIT License does not authorize false claims of official endorsement.

See the [third-party notices](THIRD_PARTY_NOTICES.md) for dependency attribution.

ChronoTrace is independently developed and is not affiliated with, jointly
developed by, or officially endorsed by M5Stack.
