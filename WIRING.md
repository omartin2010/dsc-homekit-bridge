# Wiring Reference

Full project context: [AI.md](AI.md)

Two supported hardware configurations are documented below. The LilyGO T-Internet-POE is the current build; the generic ESP32 + W5500 is the original prototype.

---

## DSC Panel Terminals (both boards)

| DSC terminal | Wire color (typical) | Signal |
|---|---|---|
| AUX+ | Red | 12V power |
| AUX− | Black | GND |
| CLK | Yellow | Keybus clock |
| DATA | Green | Keybus data |

The Keybus runs at 12V logic. Both CLK and DATA must be stepped down to 3.3V before connecting to ESP32 GPIO pins.

### Voltage divider (per line)

```
DSC line ──[20kΩ]──[20kΩ]──┬──► ESP32 GPIO (input)
                            │
                          [10kΩ]
                            │
                           GND
```

Gives ~2.4V at 12V panel / ~2.76V at 13.8V (charged battery) — safe for 3.3V GPIO.

---

## Board A: LilyGO T-Internet-POE V2 (current build)

ESP32-WROOM-32E with onboard LAN8720A RMII Ethernet and IEEE 802.3af PoE.

### Components

| Component | Value / Part | Purpose |
|-----------|-------------|---------|
| LilyGO T-Internet-POE V2 | ESP32-WROOM-32E + LAN8720A | Main MCU + onboard Ethernet + PoE |
| NPN transistor | 2N3904 or 2N2222 | Keybus write line driver |
| Resistor | 1kΩ | Transistor base current limiter |
| Resistors ×4 | 20kΩ | Voltage divider top (2 per Keybus line) |
| Resistors ×2 | 10kΩ | Voltage divider bottom (1 per Keybus line) |

### Power

**PoE only.** Power the board directly from the network switch (IEEE 802.3af). No buck converter or external supply needed.

The V2 board has onboard 1N5819 Schottky diodes OR-ing the PoE and USB-C power inputs — no external protection diode required. USB-C and PoE can be connected simultaneously (e.g., during programming) without risk of back-feed.

### DSC Keybus → ESP32 GPIO

| Signal | GPIO | Wiring |
|--------|------|--------|
| CLK in | 32 | Via 40kΩ+10kΩ voltage divider |
| DATA in | 33 | Via 40kΩ+10kΩ voltage divider |
| DATA out | 4 | NPN base via 1kΩ; collector → DSC DATA wire |

**DATA out — NPN transistor:**
```
GPIO4 ──[1kΩ]──► Base  (2N3904, flat face toward you: E − B − C)
                 Collector ──► DSC DATA wire (shared with DATA in)
                 Emitter   ──► GND
```

When GPIO4 goes HIGH the transistor pulls DATA LOW, simulating a keypad write.

### LAN8720A Ethernet (onboard — no external wiring)

These GPIOs are consumed by the RMII peripheral. Do not connect anything to them.

| Signal | GPIO |
|--------|------|
| MDC | 23 |
| MDIO | 18 |
| ETH RESET | 5 |
| CLK out (50 MHz ref) | 17 |
| RMII data bus | 19, 21, 22, 25, 26, 27 |

### GPIO Summary

| GPIO | Dir | Connected to |
|------|-----|-------------|
| 32 | In | DSC CLK (via 40kΩ+10kΩ divider) |
| 33 | In | DSC DATA in (via 40kΩ+10kΩ divider) |
| 4 | Out | 2N3904 base via 1kΩ (DATA out) |
| 5 | Out | LAN8720A ETH RESET |
| 17 | Out | LAN8720A 50 MHz ref clock |
| 18 | I/O | LAN8720A MDIO |
| 19, 21, 22, 25, 26, 27 | — | LAN8720A RMII (reserved) |
| 23 | Out | LAN8720A MDC |

---

## Board B: Generic ESP32 + W5500 (original prototype)

Standard ESP32 dev kit (HiLetgo CP2102) with an external W5500 SPI Ethernet module.

### Components

| Component | Value / Part | Purpose |
|-----------|-------------|---------|
| ESP32 dev kit | HiLetgo, CP2102 USB-serial | Main MCU |
| W5500 module | — | Wired Ethernet (SPI) |
| LM2596 buck converter | — | 12V → 5V |
| Schottky diode | 1N5817 or SS14 | USB back-feed protection |
| NPN transistor | 2N3904 or 2N2222 | Keybus write line driver |
| Resistor | 1kΩ | Transistor base current limiter |
| Resistors ×4 | 20kΩ | Voltage divider top (2 per Keybus line) |
| Resistors ×2 | 10kΩ | Voltage divider bottom (1 per Keybus line) |

### Power

```
DSC Aux 12V ──► LM2596 IN+       LM2596 OUT+ ──► [Schottky] ──┬──► ESP32 VIN (5V)
DSC Aux GND ──► LM2596 IN−       LM2596 OUT− ──► GND           └──► W5500 VCC (5V)
```

W5500 uses its onboard regulator to step 5V → 3.3V internally.

### DSC Keybus → ESP32 GPIO

| Signal | GPIO | Wiring |
|--------|------|--------|
| CLK in | 32 | Via 40kΩ+10kΩ voltage divider |
| DATA in | 33 | Via 40kΩ+10kΩ divider |
| DATA out | 21 | NPN base via 1kΩ; collector → DSC DATA wire |

**DATA out — NPN transistor:**
```
GPIO21 ──[1kΩ]──► Base  (2N3904, flat face toward you: E − B − C)
                  Collector ──► DSC DATA wire (shared with DATA in)
                  Emitter   ──► GND
```

### W5500 Ethernet → ESP32 (SPI)

| W5500 pin | ESP32 GPIO | Notes |
|-----------|-----------|-------|
| SCLK | 18 | VSPI SCK |
| MISO | 19 | VSPI MISO |
| MOSI | 22 | VSPI MOSI |
| SCS (CS) | 5 | Chip select |
| INT | 4 | Set `W5500_IRQ_PIN -1` in code if not wired |
| RESET | — | Disconnected; module pull-up holds RESET HIGH |
| VCC | 5V rail | Via Schottky cathode |
| GND | GND | Common ground |

### GPIO Summary

| GPIO | Dir | Connected to |
|------|-----|-------------|
| 32 | In | DSC CLK (via 40kΩ+10kΩ divider) |
| 33 | In | DSC DATA in (via 40kΩ+10kΩ divider) |
| 21 | Out | 2N3904 base via 1kΩ (DATA out) |
| 18 | Out | W5500 SCLK |
| 19 | In | W5500 MISO |
| 22 | Out | W5500 MOSI |
| 5 | Out | W5500 CS |
| 4 | In | W5500 INT |
