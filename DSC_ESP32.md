# DSC PC1832 — ESP32 Firmware Spec

**Board:** ESP32 with W5500 Ethernet module (perfboard assembly)
**Protocol:** DSC Keybus (2-wire serial bus, 12 V signalling via level shifter)
**HomeKit bridge:** HomeSpan (partition as security system accessory)
**Schema type:** `security`

Read `SHARED_CONTRACT.md` first (lives in `~/src/home-automation-spa`). This 
document only covers what is unique to the DSC alarm system. All universal 
endpoints, CORS, timestamp, and SPIFFS conventions are inherited from the 
shared contract.

---

## What this firmware does

1. Monitors the DSC PC1832 Keybus bus — reads zone states, arm/disarm status,
   alarm events, and trouble conditions.
2. Sends arm/disarm commands to the panel via Keybus (PIN-based).
3. Exposes the panel as a HomeKit `SecuritySystem` accessory via HomeSpan.
4. Stores notification rules (configurable via SPA) that define which zone
   events generate HomeKit notifications or activity log entries.
5. Logs all panel events, arm/disarm transitions, and zone triggers to the
   activity log.

Unlike Tekmar and Hunter, this device has **no persistent schedule config**.
The SPA shows a live status panel + notification rules editor rather than a
schedule editor. The `/api/config` endpoint stores only notification rules.

---

## Config schema (`GET /api/config` · `POST /api/config`)

The config for this device is notification rules only — not a schedule.

```json
{
  "rules": [
    {
      "id":        "r1",
      "name":      "Night perimeter",
      "enabled":   true,
      "zone_ids":  [1, 2, 3],
      "time_from": "23:00",
      "time_to":   "06:00",
      "days_mask": 127,
      "action":    "log_warn"
    },
    {
      "id":        "r2",
      "name":      "Motion — away mode",
      "enabled":   true,
      "zone_ids":  [4, 5],
      "time_from": "00:00",
      "time_to":   "23:59",
      "days_mask": 127,
      "action":    "log_warn"
    }
  ],
  "panel": {
    "auto_arm_away": false,
    "entry_delay_s": 30,
    "exit_delay_s":  45
  }
}
```

### Field rules
- `zone_ids`: array of DSC zone numbers (1-indexed, 1–8 for PC1832).
- `action`: one of `"log_info"` | `"log_warn"` | `"log_error"`.
  Future: `"homekit_notify"` when HomeSpan notification support is available.
- `time_from` / `time_to`: same `"HH:MM"` 24-hour format as other devices.
- `panel.auto_arm_away`: reserved for future use; store but do not act on it.
- `entry_delay_s` / `exit_delay_s`: informational only (reflects panel hardware
  DIP switch settings). Firmware does not control these — they are stored for
  SPA display purposes only.

---

## Status summary (`GET /api/status` → `summary` field)

```json
{
  "summary": {
    "arm_state":   "disarmed",
    "alarm_active": false,
    "trouble":      false,
    "zones": [
      { "id": 1, "name": "Front door",    "open": false, "bypassed": false },
      { "id": 2, "name": "Back door",     "open": false, "bypassed": false },
      { "id": 3, "name": "Living motion", "open": false, "bypassed": false },
      { "id": 4, "name": "Basement",      "open": false, "bypassed": false }
    ],
    "keybus_ok":    true,
    "last_event":   "Disarmed by user — 07:42"
  }
}
```

### `arm_state` values
`"disarmed"` | `"armed_stay"` | `"armed_away"` | `"arming"` | `"entry_delay"` | `"alarm"`

`keybus_ok` is `false` if the Keybus connection has not received a valid
message within the last 30 s.

---

## Arm/disarm endpoint

```
POST /api/command
```

**Request body:**
```json
{ "cmd": "arm_away", "pin": "1234" }
```

**`cmd` values:**
- `"arm_away"` — full arm
- `"arm_stay"` — perimeter arm
- `"disarm"` — disarm (PIN required)

**Response:**
```json
{ "ok": true, "sent_at": "2025-11-15T08:01:00" }
```

The PIN is transmitted to the Keybus — firmware does not validate it locally.
The panel accepts or rejects it. After sending, firmware waits up to 5 s for a
state change on the bus and reports the resulting `arm_state` in the activity log.

Log all arm/disarm commands with `source: "api"` regardless of outcome.

> **Security note:** This endpoint is LAN-only and the panel enforces PIN
> validation. Do not expose this endpoint outside the local network.
> PIN is transmitted in plaintext over HTTP — acceptable for a trusted LAN,
> but do not deploy this on an untrusted network.

---

## WebSocket live push (`ws://dsc.local/ws`)

Same pattern as the shared contract § "WebSocket status push", using
`AsyncWebSocket` from `ESPAsyncWebServer` (no extra library).

### Why this matters for DSC specifically

Every Keybus event — zone open/close, arm state transition, alarm trigger,
trouble flag — already fires a discrete interrupt-driven callback in the
`dscKeybusInterface` library. The firmware has the event the instant the panel
sends it. Without a WebSocket, the SPA polls every 30 s and can miss a door
opening for up to half a minute.

### When to push

The natural push trigger is `dsc.statusChanged` in the main loop — this already
consolidates every panel event into one check. No deferred-flag pattern is needed
because these state changes happen outside HomeSpan callbacks:

```cpp
void loop() {
  homeSpan.poll();
  dsc.loop();

  if (dsc.statusChanged) {
    dsc.statusChanged = false;
    // ... update HAP characteristics (setVal calls) ...
    wsPush();   // safe here — not inside update(), values are already committed
  }

  // 10 s heartbeat: keeps keybus_ok / last_event fresh on the SPA
  if (millis() - sLastWsPushMs >= 10000) {
    sLastWsPushMs = millis();
    ws.cleanupClients();
    wsPush();
  }
}
```

### HomeKit arm/disarm caveat

`SecuritySystemTargetState` writes come through HomeSpan's `update()` callback
where `getVal()` is still pre-commit. However, for DSC the confirmed state change
only arrives when the panel responds over Keybus (up to 5 s later), so the push
should NOT happen in `update()` anyway — it will happen naturally when
`dsc.statusChanged` fires with the new `arm_state`. No deferred flag needed.

### Arduino sketch additions

```cpp
AsyncWebSocket ws("/ws");

void wsPush() {
  ws.cleanupClients();
  if (ws.count() == 0) return;
  ws.textAll(buildStatusJson());  // same fn used by GET /api/status
}

// In setup():
ws.onEvent([](AsyncWebSocket* s, AsyncWebSocketClient* c,
              AwsEventType t, void*, uint8_t*, size_t) {
  if (t == WS_EVT_CONNECT)                         c->text(buildStatusJson());
  if (t == WS_EVT_DISCONNECT || t == WS_EVT_ERROR) s->cleanupClients();
});
server.addHandler(&ws);
```

---

## Keybus implementation

The DSC Keybus is a 2-wire bus (CLK + DATA) with 12 V signalling.
Use a resistor divider or level shifter to interface with ESP32 3.3 V GPIO.

Reference implementation:
- `taligentx/dscKeybusInterface` (Arduino library) — handles all Keybus
  framing, message parsing, and command sending. Use this library rather
  than implementing the protocol from scratch.

Key library calls used by this firmware:
```cpp
dsc.loop();                    // call in main loop — parses incoming bus data
dsc.statusChanged              // true when any panel state has changed
dsc.partitionArmed[0]          // arm state for partition 1
dsc.openZones[0]               // bitmask of open zones 1–8
dsc.alarmZones[0]              // bitmask of zones in alarm
dsc.write("*1");               // send keypad sequence (arm away example)
```

Interrupt-driven clock input is required — attach CLK to an interrupt-capable
GPIO (e.g. GPIO18) and configure the library accordingly.

---

## HomeKit accessories (HomeSpan)

Expose one `SecuritySystem` service for partition 1:

- `SecuritySystemCurrentState`:
  - `0` = stay arm
  - `1` = away arm
  - `2` = night arm (map to away arm for PC1832)
  - `3` = disarmed
  - `4` = alarm triggered
- `SecuritySystemTargetState`: writable. Writing triggers `POST /api/command`
  internally. PIN must be stored in firmware (not in config — hardcoded or
  stored in a separate secrets file not committed to source control).

Expose each zone as a `ContactSensor` service:
- `ContactSensorState`: `0` = closed (secure), `1` = open (triggered).
- `StatusTamper`: set to `1` if zone is in alarm state.

Log HomeKit arm/disarm commands with `source: "homekit"`.

---

## Activity log sources for this device

| `source`    | When written                                              |
|------------|-----------------------------------------------------------|
| `keybus`    | Zone open/close, arm state change, trouble, alarm event   |
| `rule`      | Notification rule triggered                               |
| `api`       | Arm/disarm command received via POST /api/command         |
| `homekit`   | HomeKit arm/disarm command                                |
| `system`    | Boot, NTP sync, Keybus connection lost/restored, OTA      |

---

## PlatformIO `platformio.ini` starting point

```ini
[env:dsc]
platform  = espressif32
board     = esp32dev
framework = arduino
lib_deps  =
    HomeSpan
    ESPAsyncWebServer-esphome
    ArduinoJson@^7
    dscKeybusInterface
monitor_speed = 115200
```

---

## Hardware notes

- **W5500 Ethernet module wiring (SPI):** MOSI=GPIO23, MISO=GPIO19, SCK=GPIO18,
  CS=GPIO5 (adjust to your perfboard layout).
- **Keybus level shifter:** The Keybus DATA line is 12 V. Use a voltage divider
  (e.g. 22 kΩ / 10 kΩ) to bring it to 3.3 V for the ESP32 GPIO input.
  The DSC CLK line is also 12 V — same treatment.
- **Do not connect Keybus lines directly to ESP32 GPIO** — 12 V will destroy the GPIO.
- **LM2596 buck converter:** Powers the ESP32 from the panel's 12 V aux supply.
  Set output to 5 V, feed into ESP32 VIN. Confirm output voltage before
  connecting to the board.
- **Bench supply (Korad KA3005DE):** Use current limiting (< 500 mA) when
  powering the assembly for the first time to protect against wiring errors.
- The soldered perfboard assembly is more fragile than the Hunter enclosure.
  Once validated on the bench, consider potting the perfboard or moving to
  a proper PCB for long-term deployment.
