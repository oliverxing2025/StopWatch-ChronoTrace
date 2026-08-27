# Security Policy

## Supported version

Security fixes are provided for the latest published ChronoTrace release.

## Reporting a vulnerability

Use GitHub's **Report a vulnerability** form in the repository Security tab.
Do not open a public issue containing credentials, device backups, serial
numbers, private locations, or exploit details. Include the affected version,
hardware, reproduction steps, and impact. The maintainer will acknowledge a
report as soon as practical and coordinate disclosure after a fix is available.

## Device security notes

- Wi-Fi credentials are stored locally in ESP32 NVS. The current release does
  not enable NVS encryption or Flash Encryption, so physical access to the
  device can expose stored data.
- The Wi-Fi setup page intentionally keeps the password field visible as plain
  text so it can be checked on the provisioning device. Configure it only in a
  private setting where the screen cannot be observed or recorded.
- Provisioning uses a temporary WPA2 access point and closes after credentials
  are saved or the setup timeout expires.
- Weather requests use HTTPS. Approximate location and weather data are still
  subject to the privacy practices of the named third-party services.
- Microphone samples used for music reactivity are processed locally and are
  not uploaded by ChronoTrace.

See [docs/PRIVACY.md](docs/PRIVACY.md) for the full data-flow description.
