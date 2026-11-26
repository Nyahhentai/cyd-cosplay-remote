# cyd-cosplay-remote

BLE remote for my cosplay sword LEDs. Works from an ESP32 CYD screen or just from a phone (Serial Bluetooth Terminal, BLE mode).

<p align="center">
  <img src="./assets/cyd-remote-demo.jpg" alt="CYD remote and sword" width="520" />
</p>

## Code

- [`cyd_remote/cyd_remote.ino`](./cyd_remote/cyd_remote.ino) — ESP32-2432S028R (CYD 2.8", `LovyanGFX`). Controls up to 3 BLE dimmers, shows battery %, 12 effects (`STATIC`, `BREATHE`, `HEART`, `FLICKER`, `STROBE`, `FADE`, `IGNITE`, `RETRACT`, `GLITCH`, `ERROR`, `BROWNOUT`, `SPARK`).
- [`led_dimmer_c3/led_dimmer_c3.ino`](./led_dimmer_c3/led_dimmer_c3.ino) — ESP32-C3 SuperMini receiver inside the sword hilt. 5V LED strip via `IRLB8721` MOSFET + 100k/100k battery divider.

## Receiver wiring

```text
GPIO0 -> 220R -> IRLB8721 Gate (10k to GND), Drain -> LED-, Source -> GND
GPIO1 -> 100k/100k divider from BAT+ (+ 100nF to GND)
```

## Build settings (Arduino IDE)

- **CYD:** board `ESP32 Dev Module`, partition `Huge APP (3MB No OTA/1MB SPIFFS)`, library `LovyanGFX`. Hold screen on boot to recalibrate touch.
- **ESP32-C3:** board `ESP32C3 Dev Module` (`esp32` 3.x), `USB CDC On Boot: Enabled` for Serial.
- **BLE commands** (Nordic UART `6E400001-...`): `0`..`100`, `on`, `off`, `?`.
