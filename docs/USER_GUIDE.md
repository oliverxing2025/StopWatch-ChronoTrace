# ChronoTrace User Guide

Version: `1.0.0`
Device: M5Stack StopWatch

## 1. Quick start

After boot, particles settle into a pool near the bottom of the display and react
to gravity as you rotate the device. Hold A + B to open Settings and configure
language, brightness, volume, haptics, music reactivity, Wi-Fi, weather, and
Bluetooth time calibration.

For an on-device summary, open **Settings → Operation Guide**. Its four pages cover
Buttons, Touch, Shapes, and Setup. Swipe horizontally or use A / B to change pages.

## 2. Physical buttons

| Input | Action |
| --- | --- |
| A short | Open or leave the drawing editor. |
| A double | Open or close the countdown. |
| A long | Change particle density. |
| B short | Change the particle theme. |
| B double | Show battery and charging status. |
| B long | Toggle music reactivity. |
| A + B long | Open or close Settings. |

Inside Operation Guide, A / B changes pages. Hold A + B to leave the guide or Settings.

## 3. Default scene and particle clock

| Gesture | Action |
| --- | --- |
| Tap | Assemble the current time. |
| Long press | Enable or release the persistent particle clock. |
| Double tap | Switch digital / analogue style while the clock is visible; otherwise show a random shape. |
| Swipe left | Open particle weather. |
| Swipe right | Enter shape mode. |
| Swipe up / down | Raise / lower volume. |

When a digit changes, reusable particles stay in place. Only a deficit is added,
and only surplus particles fall away. All analogue-clock hand particles use the
same visible size.

## 4. Countdown

Double-press A, choose 1–60 minutes, and start the countdown. Tap to pause or
resume; double-tap to leave. Changing digits follow the same reuse/add/drop rule.
Completion uses sound and particle feedback. If silent, check Sound and Volume in Settings.

## 5. Shapes and animations

Swipe right from the default scene to enter shape mode:

- Swipe left / right to change shape pages.
- Swipe down to switch from Shapes to Animations.
- Swipe up to return from Animations to Shapes.
- Tap shapes to add them to the playlist in order.
- Hold to play the selected sequence, or preview the current item when none are selected.
- Double-tap the default scene to show a random shape.

Shapes and animations use particles already present in the pool. While an effect
runs, its independent parts do not borrow particles from each other. Animations
include directional arrows, heartbeat, DNA double helix, fireworks, particle rain,
and a horizontally scrolling “I LOVE YOU”.

## 6. Custom drawings

Press A or choose **Draw** on the shape picker. Select colors at the sides and draw
inside the central canvas. Up to 12 custom drawings can be stored.

- Back: leave the editor.
- Delete: remove the current custom drawing.
- Next: move to the next storage slot.
- Confirm: save the drawing.

Built-in shapes cannot be deleted.

## 7. Music reactivity

Hold B or enable Music Reactivity under **Settings → Device Settings**. Audio is
analyzed locally. Bass, mids, and treble drive waves, upward particle jumps, and
sparks. If particles only change color, move the audio source closer, increase its
volume, and make sure the microphone is unobstructed.

## 8. Battery and charging

Double-press B to view battery status. While charging, particles build a line below
the battery; once complete, it falls apart and forms again. After an installation,
allow the device to boot. If necessary, restart it once rather than repeatedly
disconnecting power.

## 9. Wi-Fi, weather, and time

- Wi-Fi: tap the disconnected status to search for and join a network.
- Weather: use approximate automatic location or enter a city manually.
- Bluetooth: calibrate device time from a phone.

See the [Privacy Notice](PRIVACY.md) for network details. Credentials, drawings,
and preferences are stored locally, and microphone audio is not uploaded.

## 10. Settings and About

Hold A + B for Settings. Configure language, connectivity, brightness, volume,
haptics, and music reactivity, or open Operation Guide and About. About displays
the ChronoTrace logo, version, copyright, and individual developer credit.

## 11. Troubleshooting

### The display is blank but sound works

Restart once. If the display remains blank, install an official ChronoTrace image
for M5Stack StopWatch. Do not reuse firmware or flash offsets from another ESP32-S3 device.

### Sound or particle effects are missing

Check Sound, Volume, and Music Reactivity in Settings. Program sound effects do not
require music reactivity, while beat-driven motion requires audible external audio.

### No Wi-Fi network appears

Wait for the network search to finish and retry near the router. Enable a compatible
2.4 GHz network if the router is currently using an unsupported band only.

### Removing personal data

Change normal preferences on the device. For a complete removal of locally stored
Wi-Fi and other data, use M5Stack's official erase and recovery process after
saving anything you need.

## 12. Installation and recovery

Install only from the developer's officially published M5Burner listing or Share
Code. Confirm the target is M5Stack StopWatch, use a data-capable USB cable, and
keep power connected throughout the burn. Do not guess flash addresses. Use
M5Stack's official firmware and instructions to restore the factory system.
