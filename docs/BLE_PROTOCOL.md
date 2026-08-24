# ChronoTrace BLE companion protocol

Device name: `ChronoTrace`

Service UUID: `8f620000-7b9d-4b8a-a3c4-435452414345`

## Time write

Characteristic UUID: `8f620001-7b9d-4b8a-a3c4-435452414345`

Write exactly 10 bytes, little-endian:

- bytes 0–7: unsigned Unix UTC seconds
- bytes 8–9: signed timezone offset in minutes

The firmware writes the corresponding local date and time to the RX8130CE RTC.
A companion should write once immediately after connection and may repeat while
connected (for example every six hours). The firmware applies every valid packet.

## iPhone behavior

iOS Settings can pair/connect BLE devices but cannot provide a custom text-entry
screen for a GATT characteristic. A ChronoTrace companion app (or a generic BLE
tool such as nRF Connect/LightBlue for engineering tests) must discover this
service and write the time packet. ChronoTrace exposes no phone-text channel.
