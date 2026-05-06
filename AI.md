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
| ESP32 dev kit (HiLetgo) — CP2102 USB-serial chip (Silicon Labs) | Main microcontroller |
| W5500 Ethernet module | Wired LAN (no WiFi) |
| LM2596 buck converter | Steps 12V panel supply → 5V for ESP32 |
| 2N2222 or 2N3904 NPN transistor | Keybus write line driver |
| 4.7kΩ resistors ×2 | Pull-ups on Keybus CLK and DATA lines |
| 10kΩ resistor ×1 | Transistor base resistor |
| Schottky diode (1N5817 or SS14) | Power rail protection (see Power section) |
| Perfboard + project enclosure | Permanent mount |

## Power

- Tapped from DSC panel 12V auxiliary supply
- Stepped down via LM2596 on perfboard → 5V rail
- Schottky diode (1N5817 or SS14) in series on the 5V rail: anode to LM2596 output, cathode to the shared 5V node (ESP32 VIN + W5500 VCC)
  - Prevents USB back-feed into the LM2596 when USB is simultaneously connected (e.g. for serial monitoring)
  - Both ESP32 and W5500 are powered from the cathode (protected) side
- W5500 powered from 5V (its onboard regulator steps down to 3.3V internally), offloading the ESP32's onboard LDO
- No external USB adapter needed

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
| Write (DATA out) | 21 | NPN base via 1kΩ, collector to DSC Green |

> GPIOs 18/19 are reserved for W5500 SPI (moved from the dscKeybusInterface defaults of 18/19).

### W5500 Ethernet (SPI)

| W5500 Signal | ESP32 GPIO | Notes |
|---|---|---|
| SCLK | 18 | Default VSPI SCK |
| MISO | 19 | Default VSPI MISO |
| MOSI | 22 | — |
| SCS (CS) | 5 | Chip select |
| INT | 4 | Set `W5500_IRQ_PIN -1` if not wired |
| RESET | 0 | GPIO 0 is LOW in download mode → W5500 held in reset during upload |

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

## Code Conventions

- Language: C++ (Arduino framework)
- Target board: ESP32 (generic dev kit)
- Networking: W5500 via SPI (Ethernet, not WiFi)
- Keep WiFi stack disabled to save RAM
- Prefer clean separation: `dsc.*` files for panel protocol, `homekit.*` for HomeKit accessory logic
- No cloud calls, no telemetry
