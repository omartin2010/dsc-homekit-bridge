# DSC PC1832 — ESP32 Firmware Spec

**Board:** LilyGO T-Internet-POE (ESP32-WROOM-32E + onboard LAN8720A RMII PHY + PoE)
**Protocol:** DSC Keybus (2-wire serial bus, 12 V signalling via resistor dividers)
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
    "exit_delay_s":  45,
    "access_code":   "",
    "log_motion":    false,
    "chime_enabled": true
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
- `panel.access_code`: 4–8 digit string. When non-empty, used as the PIN for
  `POST /api/command` validation **instead of** the hardcoded value in `secrets.h`.
  Allows the SPA to configure the alarm code without reflashing.
  Validation on POST: reject if non-empty and not 4–8 digits (HTTP 422).
  Store in `/config.json`. Load on boot; fall back to `secrets.h` value if empty.
- `panel.log_motion`: boolean (default `false`). When `false`, motion-sensor zone
  open/close events are suppressed from the activity log. Toggled via Settings in SPA.
- `panel.chime_enabled`: boolean (default `true`). Persists the SPA's view of whether
  the panel entry chime is on. Updated by the SPA after a successful `chime_toggle`
  command. Firmware stores it but does **not** re-apply it on boot (no read-back from panel).

---

## Status summary (`GET /api/status` → `summary` field)

```json
{
  "device":     "dsc",
  "fw_version": "3.0",
  "uptime_s":   86400,
  "connected":  true,
  "timestamp":  "2025-11-15T07:42:00",
  "summary": {
    "keybus_connected": true,
    "arm_state":        "disarmed",
    "trouble":          false,
    "power_trouble":    false,
    "battery_trouble":  false,
    "fire":             false,
    "zones_open":       0,
    "zones": [
      {
        "number":   1,
        "name":     "Porte Avant",
        "location": "RDC",
        "type":     "door",
        "open":     false,
        "bypassed": false,
        "alarm":    false
      }
    ]
  }
}
```

Only configured zones are included (active zones: 1–8, 10–13, 17–21). See ZONES.md.

### `arm_state` values

Derived from `dscKeybusInterface` partition 1 (index 0) fields.
Priority when multiple flags are true: `alarm` > `entry_delay` > armed states > exit delays > `disarmed`.

| Value                 | Condition                                                                      |
|-----------------------|--------------------------------------------------------------------------------|
| `"disarmed"`          | `!dsc.armed[0] && !dsc.exitDelay[0] && !dsc.alarm[0]`                         |
| `"exit_delay_stay"`   | `dsc.exitDelay[0] && dsc.exitState[0] == DSC_EXIT_STAY`                        |
| `"exit_delay_away"`   | `dsc.exitDelay[0] && dsc.exitState[0] == DSC_EXIT_AWAY`                        |
| `"exit_delay_night"`  | `dsc.exitDelay[0] && dsc.exitState[0] == DSC_EXIT_NO_ENTRY_DELAY`              |
| `"exit_delay"`        | `dsc.exitDelay[0]` (fallback when exitState doesn't match any known value)     |
| `"armed_stay"`        | `dsc.armed[0] && dsc.armedStay[0] && !dsc.noEntryDelay[0]`                    |
| `"armed_away"`        | `dsc.armed[0] && dsc.armedAway[0] && !dsc.noEntryDelay[0]`                    |
| `"armed_night"`       | `dsc.armed[0] && dsc.noEntryDelay[0]`                                          |
| `"entry_delay"`       | `dsc.entryDelay[0]`                                                            |
| `"alarm"`             | `dsc.alarm[0]`                                                                 |

### Zone fields

| Field      | Source                                            | Notes                                       |
|-----------|---------------------------------------------------|---------------------------------------------|
| `number`   | Zone number (1-indexed)                           |                                             |
| `name`     | Static — from ZONES.md                            |                                             |
| `location` | Static — from ZONES.md (`RDC`, `2e`, `SS`)        |                                             |
| `type`     | Static — `door`, `window`, `motion`, `leak`, `smoke` |                                          |
| `open`     | `bitRead(dsc.openZones[(n-1)/8], (n-1)%8)`        | Zone 11: uses `dsc.fire[0]` instead         |
| `bypassed` | `gBypassedZones[n]` — optimistic firmware flag     | Toggled by `bypass` command; resets on reboot; drifts if keypad is used directly |
| `alarm`    | `bitRead(dsc.alarmZones[(n-1)/8], (n-1)%8)`       |                                             |

**`bypassed` implementation:** `gBypassedZones[33]` (1-indexed) lives in RAM and is toggled
optimistically when `POST /api/command bypass` is processed. The `dscKeybusInterface`
library does not expose a `bypassedZones[]` read-back, so this state is best-effort and
resets to all-false on firmware reboot.

---

## Arm/disarm endpoint

```
POST /api/command
```

**Request body:**
```json
{ "cmd": "arm_away", "pin": "1234" }
```

**`cmd` values and Keybus writes:**

| `cmd`           | Keybus write             | Pre-condition                                                          |
|-----------------|--------------------------|------------------------------------------------------------------------|
| `arm_stay`      | `dsc.write('s')`         | `dsc.ready[0]` must be true                                            |
| `arm_away`      | `dsc.write('w')`         | `dsc.ready[0]` must be true                                            |
| `arm_night`     | `dsc.write('n')`         | `dsc.ready[0]` must be true (arm with no entry delay)                  |
| `disarm`        | `dsc.write(pin)`         | `dsc.armed[0] \|\| dsc.exitDelay[0] \|\| dsc.alarm[0]`                |
| `alarm_reset`   | `dsc.write('r')`         | none — clears alarm memory after a silenced alarm                      |
| `chime_toggle`  | `dsc.write('c')`         | none — toggles the panel entry/exit chime; state is not read back      |
| `bypass`        | `dsc.write("*1NN#")`     | `!dscArmed()` — NN is zero-padded zone number (e.g. `*105#` for zone 5) |

The `bypass` command requires an additional `"zone"` field (integer 1–32):
```json
{ "cmd": "bypass", "zone": 5, "pin": "1234" }
```
Response includes the new bypass state:
```json
{ "ok": true, "bypassed": true }
```
Bypass toggles: sending the same command again un-bypasses the zone.

Set `dsc.writePartition = 1` before each write.

**Response (success, non-bypass commands):**
```json
{ "ok": true }
```

**Response (wrong PIN — mismatch against stored access code):**
```json
{ "ok": false, "error": "Unauthorized" }
```
HTTP 403. The firmware validates the PIN against the stored `accessCode` from `secrets.h`
before issuing any Keybus write. The panel's own PIN check is a second layer.

**Response (panel not ready for arm):**
```json
{ "ok": false, "error": "Panel not ready" }
```
HTTP 422.

Log all commands with `source: "api"` regardless of outcome.

> **Security note:** This endpoint is LAN-only. PIN is transmitted in plaintext
> over HTTP — acceptable for a trusted LAN, but do not expose outside the local network.

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

Key library fields used by this firmware:

```cpp
dsc.loop();                    // call in main loop — parses incoming bus data
dsc.statusChanged              // true when any panel state has changed
dsc.keybusConnected            // true if Keybus data is being received
dsc.armed[0]                   // partition 1 armed (any mode)
dsc.armedAway[0]               // armed away
dsc.armedStay[0]               // armed stay
dsc.noEntryDelay[0]            // night arm (no entry delay)
dsc.exitDelay[0]               // exit delay in progress
dsc.exitState[0]               // DSC_EXIT_STAY / DSC_EXIT_AWAY / DSC_EXIT_NO_ENTRY_DELAY
dsc.entryDelay[0]              // entry delay in progress
dsc.alarm[0]                   // alarm triggered
dsc.fire[0]                    // fire alarm triggered
dsc.ready[0]                   // panel ready to arm
dsc.trouble                    // general trouble
dsc.powerTrouble               // AC power trouble
dsc.batteryTrouble             // battery trouble
dsc.openZones[group]           // bitmask: bit N = zone (group*8 + N + 1) is open
dsc.alarmZones[group]          // bitmask: zones in alarm
dsc.write('s');                // stay arm keypad key
dsc.write('w');                // away arm keypad key
dsc.write(accessCode);         // disarm
dsc.writePartition = 1;        // target partition for next write
```

Zone group formula: `group = (zoneNumber - 1) / 8`, `bit = (zoneNumber - 1) % 8`.

**Keypad write keys used by this firmware:**
- `'s'` (0xAF) — stay arm
- `'w'` (0xB1) — away arm
- `'n'` (0xB6) — night arm (no entry delay)
- `'r'` (0xDA) — alarm reset / clear alarm memory
- `'c'` (0xBB) — door chime toggle
- `"*1NN#"` string — zone bypass toggle (NN = zero-padded zone number)
- PIN string — disarm

**Not available from library:** read-back of bypassed zones; `gBypassedZones[]` is optimistic only.

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

| `source`    | When written                                                                  |
|------------|-------------------------------------------------------------------------------|
| `keybus`    | Zone open/close, arm state change, trouble, alarm event                       |
| `rule`      | Notification rule triggered                                                   |
| `api`       | Any command received via POST /api/command (arm, disarm, bypass, chime, etc.) |
| `homekit`   | HomeKit arm/disarm command                                                    |
| `system`    | Boot, NTP sync, Keybus connection lost/restored, OTA                          |

Activity log buffer: **200 entries** (`ACTIVITY_MAX_ENTRIES` in `activity_log.h`).
The SPA fetches up to 100 entries (`/api/activity?limit=100`) for the History charts.

---

## PlatformIO `platformio.ini` starting point

See the project's `platformio.ini` for the actual build config.
The two environments are `t_internet_poe` (USB flash) and `t_internet_poe_ota` (OTA upload).

Key library dependencies:
```
HomeSpan
ESPAsyncWebServer
AsyncTCP
ArduinoJson@^7
dscKeybusInterface
```

---

## Hardware notes

See `AI.md` for full wiring, pin assignments, and power supply details.

The board is the **LilyGO T-Internet-POE** (ESP32-WROOM-32E + onboard LAN8720A RMII PHY).
Ethernet is onboard — no W5500 SPI module. Use `ETH.begin()` with RMII parameters
(see `main.cpp`) rather than the SPI Ethernet library.

Key Keybus wiring:
- CLK → GPIO 32 (via resistor divider: 12 V → 3.3 V)
- DATA read → GPIO 33 (via resistor divider)
- DATA write → GPIO 4 → NPN base (1 kΩ), collector to DSC DATA line
- GPIO 21 is reserved for RMII TX_EN — do not use for Keybus write
