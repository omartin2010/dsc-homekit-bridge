# Wiring Reference

Full project context: [AI.md](AI.md)

## Components

| Component | Value / Part | Purpose |
|-----------|-------------|---------|
| ESP32 dev kit | HiLetgo, CP2102 USB-serial | Main microcontroller |
| W5500 module | — | Wired Ethernet (SPI) |
| Buck converter | LM2596 | 12V → 5V |
| Schottky diode | 1N5817 or SS14 | USB back-feed protection |
| NPN transistor | 2N3904 (or 2N2222) | Keybus write line driver |
| Resistor | 1kΩ | Transistor base current limiter |
| Resistors ×4 | 20kΩ | Voltage divider top (2 per line) |
| Resistors ×2 | 10kΩ | Voltage divider bottom (1 per line) |

---

## Power

```
DSC Aux 12V ──► LM2596 IN+       LM2596 OUT+ ──► [Schottky diode] ──┬──► ESP32 VIN (5V)
DSC Aux GND ──► LM2596 IN−       LM2596 OUT− ──► GND                 └──► W5500 VCC (5V)
                                                                      
                                                       All GND → common ground
```

- Schottky diode: anode to LM2596 output, cathode to the shared 5V node
- Prevents USB back-feed into LM2596 when USB is simultaneously connected
- W5500 uses its onboard regulator to step 5V → 3.3V internally

---

## DSC Keybus → ESP32

### DSC wiring to panel terminals

| DSC terminal | Wire color (typical) | Signal |
|---|---|---|
| AUX+ | Red | 12V power |
| AUX− | Black | GND |
| CLK | Yellow | Keybus clock |
| DATA | Green | Keybus data |

### CLK line — voltage divider → GPIO32

The DSC Keybus runs at 12V; the ESP32 GPIO is 3.3V max. A resistor divider steps the voltage down safely (~2.4V at 12V panel, ~2.76V at 13.8V charged battery).

```
DSC CLK ──[20kΩ]──[20kΩ]──┬──► GPIO32
                           │
                         [10kΩ]
                           │
                          GND
```

- Total series resistance: 40kΩ (two 20kΩ in series)
- Bottom resistor: 10kΩ to GND

### DATA IN line — voltage divider → GPIO33

Same divider as CLK.

```
DSC DATA ──[20kΩ]──[20kΩ]──┬──► GPIO33
                            │
                          [10kΩ]
                            │
                           GND
```

### DATA OUT line — NPN transistor → GPIO21

The ESP32 cannot drive the 12V Keybus data line directly. An NPN transistor (2N3904) acts as a switch: when GPIO21 goes HIGH, the transistor pulls the DATA line LOW, simulating a keypad response.

The DSC DATA terminal and the collector connect to the **same wire** as DATA IN — the Keybus data line is bidirectional.

```
GPIO21 ──[1kΩ]──► Base  (middle pin, flat face toward you)
                  Collector ──► DSC DATA wire (shared with DATA IN)
                  Emitter   ──► GND
```

**2N3904 pinout** (TO-92, flat face toward you, leads down): E − B − C (left to right)

---

## W5500 Ethernet → ESP32 (SPI)

| W5500 pin | ESP32 GPIO | Notes |
|-----------|-----------|-------|
| SCLK | 18 | VSPI SCK |
| MISO | 19 | VSPI MISO |
| MOSI | 22 | — |
| SCS (CS) | 5 | Chip select |
| INT | 4 | Set `W5500_IRQ_PIN -1` in code if not wired |
| RESET | — | Disconnected from GPIO0; module's onboard pull-up holds RESET HIGH |
| VCC | 5V rail | Via Schottky cathode |
| GND | GND | Common ground |

---

## ESP32 GPIO Summary

| GPIO | Direction | Connected to |
|------|-----------|-------------|
| 32 | Input | DSC CLK (via 40kΩ+10kΩ divider) |
| 33 | Input | DSC DATA IN (via 40kΩ+10kΩ divider) |
| 21 | Output | 2N3904 base via 1kΩ (DATA OUT) |
| 18 | Output | W5500 SCLK |
| 19 | Input | W5500 MISO |
| 22 | Output | W5500 MOSI |
| 5 | Output | W5500 CS |
| 4 | Input | W5500 INT |
| 0 | — | W5500 RESET disconnected; GPIO0 free (BOOT button only) |
