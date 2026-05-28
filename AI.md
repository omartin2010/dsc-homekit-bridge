# DSC Alarm Panel → HomeKit/Google Home via ESP32 + Ethernet

## Project Goal

Integrate a DSC PC1832 PowerSeries alarm panel with Apple HomeKit and/or Google Home using an ESP32 microcontroller and the `dscKeybusInterface` Arduino library. No cloud subscription, no Alarm.com, no monthly fees.

## Key Library

- **dscKeybusInterface**: https://github.com/taligentx/dscKeybusInterface
- Supports: arm/disarm, zone status, fire alarms
- Integration targets: HomeKit, Google Home (Homebridge), Home Assistant, MQTT

## Hardware

| Component | Purpose |
|-----------|---------|
| LilyGO T-Internet-POE (ESP32-WROOM-32E + LAN8720A) | Main microcontroller + onboard Ethernet + PoE |
| LM2596 buck converter | Steps 12V panel supply → 5V (if not using PoE) |
| 2N2222 or 2N3904 NPN transistor | Keybus write line driver |
| 4.7kΩ resistors ×2 | Pull-ups on Keybus CLK and DATA lines |
| 10kΩ resistor ×1 | Transistor base resistor |
| Perfboard + project enclosure | Permanent mount |

> No external W5500 module needed — Ethernet is onboard via LAN8720A RMII PHY.

## Power

Two options:
1. **PoE** — power the T-Internet-POE directly from the network switch (IEEE 802.3af). Simplest wiring, no LM2596 needed.
2. **Panel 12V** — tap DSC aux 12V supply → LM2596 buck converter → 5V into board VIN pin.

## Wiring: DSC Panel → ESP32 Box (4-wire bundle)

| Wire | Signal |
|------|--------|
| 12V | Panel aux power |
| GND | Common ground |
| CLK | Keybus clock line |
| DATA | Keybus data line |

## ESP32 Pin Assignments

### DSC Keybus

| Signal | GPIO | Notes |
|--------|------|-------|
| Clock (CLK) | 32 | DSC Yellow wire via resistor divider |
| Read (DATA in) | 33 | DSC Green wire via resistor divider |
| Write (DATA out) | 4 | NPN base via 1kΩ, collector to DSC Green |

> GPIO 21 is reserved by the LAN8720 RMII peripheral (TX_EN) — write pin moved from 21 → 4.

### LAN8720A Ethernet (RMII — onboard, no wiring needed)

| Signal | GPIO | Notes |
|--------|------|-------|
| MDC | 23 | Management clock |
| MDIO | 18 | Management data |
| ETH RESET | 5 | |
| CLK out | 17 | 50 MHz ref clock to PHY (`ETH_CLOCK_GPIO17_OUT`) |
| RMII data | 19, 21, 22, 25, 26, 27 | Reserved — do not use for other purposes |

## Physical Build

- All components soldered on perfboard inside small project enclosure
- Ethernet cable from box to nearby network switch
- 4-wire bundle routed from box to DSC panel terminals

## Zone Layout

Configured zones are documented in [ZONES.md](ZONES.md). That file is the source of truth for generating the sketch's accessory list — zone numbers, HomeKit types, descriptions, and floor locations.

Active zones: 1–8, 10–13, 17–21. Zones 9, 14–16, and 22–32 are unused.

## Integration Path

### Apple HomeKit (native, no hub)
- Library: **HomeSpan** (https://github.com/HomeSpan/HomeSpan)
- Adapt the `dscKeybusInterface` HomeKit example sketch for W5500 Ethernet instead of WiFi
- Pair ESP32 directly with Apple Home app as a native accessory
- No Homebridge, no MQTT broker, no Home Assistant required

## Development Setup

1. Arduino IDE + ESP32 board support (Espressif ESP32 package)
2. Libraries to install via Arduino Library Manager:
   - `dscKeybusInterface`
   - `HomeSpan`
   - `Ethernet` (for W5500 / WIZnet)
3. Flash adapted HomeKit-Ethernet sketch
4. Pair via Apple Home app

## Web API (ESPAsyncWebServer, port 80)

```
POST   /api/homekit/reset    → Clear HomeKit pairing data and reboot (sf=1 after reboot)
```

HAP server runs on port 8080 (`homeSpan.setPortNum(8080)`) to avoid conflict with the web server on port 80. Hostname is `dsc` (`dsc.local`).

## OTA Updates

First flash via the LilyGO downloader module plugged into the 6-pin programming header:

```bash
pio run -e t_internet_poe --target upload
```

After that, OTA is enabled. Update the IP in `platformio.ini` `[env:t_internet_poe_ota]` then:

```bash
pio run -e t_internet_poe_ota --target upload
```

HomeSpan OTA runs on the standard ArduinoOTA port (3232), no password.

## Code Conventions

- Language: C++ (Arduino framework)
- Target board: ESP32 (generic dev kit)
- Networking: W5500 via SPI (Ethernet, not WiFi)
- Keep WiFi stack disabled to save RAM
- Prefer clean separation: `dsc.*` files for panel protocol, `homekit.*` for HomeKit accessory logic
- No cloud calls, no telemetry
