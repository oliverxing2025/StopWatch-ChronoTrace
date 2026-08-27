# ChronoTrace Privacy Notice

Effective date: August 26, 2026

ChronoTrace requires no account and contains no advertising or behavior-analytics
module. Drawings, microphone audio, and particle playback content are not uploaded.
Microphone samples are processed in device memory for music reactivity.

The device may store Wi-Fi credentials, settings, drawings, colors, weather mode,
city, coordinates, and recent weather data in NVS. The current firmware does not
enable Flash Encryption, so a person with physical access may be able to read
unencrypted flash contents.

The Wi-Fi provisioning page intentionally displays the password as plain text
so the user can verify it before saving. Use this page only in a private setting
where nearby people, cameras, or screen-sharing software cannot observe it.

When Wi-Fi and weather are enabled, automatic mode contacts `ipwho.is` for an
approximate city and coordinates derived from the public IP address. Manual mode
sends the entered city to Open-Meteo geocoding. Coordinates are sent to Open-Meteo
weather services. These services can observe the request's public IP and apply
their own privacy policies; they are not operated by ChronoTrace.

Bluetooth accepts time-calibration data and does not transmit drawings, Wi-Fi
passwords, or microphone audio. Users can disable connectivity or delete drawings.
Only a recovery flow that erases NVS is guaranteed to remove locally stored
ChronoTrace data; rewriting the app alone may preserve it. Confirm backup and
recovery requirements before erasing flash.

Use GitHub's private vulnerability-reporting form for security issues. For
privacy questions, use the contact channel on the official release or developer
announcement.
