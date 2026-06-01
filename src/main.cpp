/*
 *  DSC-HomeKit-Ethernet
 *
 *  Adapted from dscKeybusInterface HomeKit-HomeSpan example for wired Ethernet.
 *  Original: https://github.com/taligentx/dscKeybusInterface/tree/master/examples/esp32/HomeKit-HomeSpan
 *
 *  Target board: LilyGO T-Internet-POE (ESP32-WROOM-32E + onboard LAN8720A RMII PHY)
 *
 *  LAN8720A RMII — all connections are onboard, no external wiring needed:
 *    PHY type:  LAN8720    PHY addr: 0
 *    MDC:       GPIO 23    MDIO:     GPIO 18
 *    ETH reset: GPIO  5    CLK out:  GPIO 17 (ETH_CLOCK_GPIO17_OUT)
 *    RMII data: GPIO 19/21/22/25/26/27 (reserved by peripheral, do not use)
 *
 *  DSC Keybus wiring (see AI.md):
 *    GPIO 32 (CLK)   ← DSC Yellow via resistor divider
 *    GPIO 33 (READ)  ← DSC Green  via resistor divider
 *    GPIO  4 (WRITE) → NPN base via 1kΩ, collector to DSC Green
 *    (GPIO 21 is RMII TX_EN — moved write pin from 21 → 4)
 */

// DSC Classic series: uncomment for PC1500/PC1550 support
//#define dscClassicSeries

#include <Arduino.h>
#include <WiFi.h>
#include <ETH.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include "HomeSpan.h"
#include <dscKeybusInterface.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "secrets.h"
#include "activity_log.h"
#include "web_api.h"

static char gHomekitCode[16] = "46637726";

// DSC Keybus pins
#define dscClockPin  32
#define dscReadPin   33
#define dscPC16Pin   16  // DSC Classic Series only
#define dscWritePin   4  // was 21; GPIO21 is RMII TX_EN on T-Internet-POE

// Initialize DSC interface
#ifndef dscClassicSeries
dscKeybusInterface dsc(dscClockPin, dscReadPin, dscWritePin);
#else
dscClassicInterface dsc(dscClockPin, dscReadPin, dscPC16Pin, dscWritePin, accessCode);
#endif

bool updatePartitions, updateZones, updatePGMs, updateSmokeSensors;

// NTP / time sync state
const byte timePartition = 1;
bool ntpSynced    = true;
bool ntpImmediate = false;
byte ntpOffset    = 0;
tm   ntpTime;
bool ntpReady     = false;
unsigned long ntpLastLog = 0;

// WebSocket heartbeat
static unsigned long sLastWsPushMs  = 0;
static unsigned long sLastFlushMs   = 0;
#define WS_PUSH_INTERVAL_MS   10000
#define FLUSH_INTERVAL_MS     30000

bool gLogMotion = false;  // log motion sensor zone events; set from panel.log_motion in config
bool gBypassedZones[33] = {};  // index = zone number (1-based); toggled optimistically on bypass cmd

#include "dscHomeSpanAccessories.h"

// ── Zone table (from ZONES.md) ────────────────────────────────────────────────
struct ZoneInfo { uint8_t number; const char *name; const char *location; const char *type; };
static const ZoneInfo kZones[] = {
  {1,  "Porte Avant",        "RDC", "door"},
  {2,  "Porte Arrière",      "SS",  "door"},
  {3,  "Porte Patio",        "RDC", "door"},
  {4,  "Porte Patio",        "2e",  "door"},
  {5,  "Corridor",           "RDC", "motion"},
  {6,  "Salle à manger",     "RDC", "motion"},
  {7,  "Salon",              "RDC", "motion"},
  {8,  "Bureau",             "2e",  "motion"},
  {10, "Master Bathroom",    "2e",  "leak"},
  {11, "Détecteur de fumée", "RDC", "smoke"},
  {12, "Fenêtre Ouest",      "RDC", "window"},
  {13, "Fenêtre Nord",       "RDC", "window"},
  {17, "Salon",              "SS",  "motion"},
  {18, "Fenêtre Atelier",    "SS",  "window"},
  {19, "Fenêtre Sud",        "SS",  "window"},
  {20, "Fenêtre Ouest",      "SS",  "window"},
  {21, "Atelier",            "SS",  "motion"},
};
static const int kZoneCount = (int)(sizeof(kZones) / sizeof(kZones[0]));

// ── Arm state helper ──────────────────────────────────────────────────────────
static const char *armStateStr(byte partition) {
  if (dsc.alarm[partition])     return "alarm";
  if (dsc.entryDelay[partition])return "entry_delay";
  if (dsc.armed[partition] && dsc.noEntryDelay[partition]) return "armed_night";
  if (dsc.armed[partition] && dsc.armedAway[partition])    return "armed_away";
  if (dsc.armed[partition] && dsc.armedStay[partition])    return "armed_stay";
  if (dsc.exitDelay[partition]) {
    if (dsc.exitState[partition] == DSC_EXIT_STAY)           return "exit_delay_stay";
    if (dsc.exitState[partition] == DSC_EXIT_AWAY)           return "exit_delay_away";
    if (dsc.exitState[partition] == DSC_EXIT_NO_ENTRY_DELAY) return "exit_delay_night";
    return "exit_delay";
  }
  return "disarmed";
}

// ── buildStatusJson — called by web_api.cpp via forward declaration ───────────
String buildStatusJson() {
  JsonDocument resp;
  resp["device"]     = "dsc";
  resp["fw_version"] = "3.0";
  resp["uptime_s"]   = (uint32_t)(millis() / 1000);
  resp["connected"]  = dsc.keybusConnected;

  time_t now = time(nullptr);
  char ts[20];
  if (now > 100000) {
    struct tm ti; localtime_r(&now, &ti);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &ti);
  } else {
    strlcpy(ts, "1970-01-01T00:00:00", sizeof(ts));
  }
  resp["timestamp"] = ts;

  JsonObject sum = resp["summary"].to<JsonObject>();
  sum["keybus_connected"] = dsc.keybusConnected;
  sum["arm_state"]        = armStateStr(0);
  sum["trouble"]          = dsc.trouble;
  sum["power_trouble"]    = dsc.powerTrouble;
  sum["battery_trouble"]  = dsc.batteryTrouble;
  sum["fire"]             = dsc.fire[0];

  int zonesOpen = 0;
  JsonArray zones = sum["zones"].to<JsonArray>();
  for (int i = 0; i < kZoneCount; i++) {
    uint8_t n = kZones[i].number;
    bool open, alarm;
    if (n == 11) {
      open = alarm = dsc.fire[0];
    } else {
      int g = (n - 1) / 8, b = (n - 1) % 8;
      open  = bitRead(dsc.openZones[g], b);
      alarm = bitRead(dsc.alarmZones[g], b);
    }
    if (open) zonesOpen++;
    JsonObject zobj = zones.add<JsonObject>();
    zobj["number"]   = n;
    zobj["name"]     = kZones[i].name;
    zobj["location"] = kZones[i].location;
    zobj["type"]     = kZones[i].type;
    zobj["open"]     = open;
    zobj["bypassed"] = (n < 33) ? gBypassedZones[n] : false;
    zobj["alarm"]    = alarm;
  }
  sum["zones_open"] = zonesOpen;

  String out;
  serializeJson(resp, out);
  return out;
}

// ── Zone change logger — call from statusChanged block, before updateZones ────
static void logZoneChanges() {
  for (int i = 0; i < kZoneCount; i++) {
    uint8_t n = kZones[i].number;
    if (n == 11) continue;
    int g = (n - 1) / 8, b = (n - 1) % 8;
    if (!bitRead(dsc.openZonesChanged[g], b)) continue;
    if (!gLogMotion && strcmp(kZones[i].type, "motion") == 0) continue;
    bool open = bitRead(dsc.openZones[g], b);
    activityLog(open ? "warn" : "info", "keybus",
                "%s (%s) %s", kZones[i].name, kZones[i].location,
                open ? "opened" : "closed");
  }
}

// ── Thin accessors for web_api.cpp (avoids re-including dscKeybusInterface.h) ─
bool dscReady()     { return dsc.ready[0]; }
bool dscArmed()     { return dsc.armed[0]; }
bool dscExitDelay() { return dsc.exitDelay[0]; }
bool dscAlarm()     { return dsc.alarm[0]; }

static void onEthEvent(arduino_event_id_t event, arduino_event_info_t) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("[ETH] Started");
      ETH.setHostname("dsc");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("[ETH] Link up");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.printf("[ETH] IP: %s\n", ETH.localIP().toString().c_str());
      // HomeSpan has already called MDNS.begin() — just register the HTTP service.
      MDNS.addService("http", "tcp", 80);
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
      Serial.println("[ETH] Lost IP");
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("[ETH] Link down");
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("[ETH] Stopped");
      break;
    default: break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();

  if (!LittleFS.begin(true))
    Serial.println("[FS] LittleFS mount failed");

  activityInit();

  WiFi.onEvent(onEthEvent);

  ETH.begin(ETH_PHY_LAN8720, /*phy_addr=*/0, /*mdc=*/23, /*mdio=*/18, /*rst=*/5, ETH_CLOCK_GPIO17_OUT);

  configTzTime(timeZone, ntpServer);

  homeSpan.setPortNum(8080);
  homeSpan.enableOTA(false);
  homeSpan.setPairingCode(gHomekitCode);
  homeSpan.begin(Category::Bridges, "DSC Security System", "dsc");
  homeSpan.setHostNameSuffix("");

  setupWebServer();

  // Bridge accessory identification
  new SpanAccessory();
    new homeSpanIdentify("DSC Security System", "DSC", "000000", "PC1832", "3.0");
    new Service::HAPProtocolInformation();
      new Characteristic::Version("1.1.0");

  // --- Zone layout: see ZONES.md ---

  // Partition 1
  new SpanAccessory();
    new homeSpanIdentify("Alarme", "DSC", "000000", "Alarm", "3.0");
    new dscPartition(1);

  // Zone 1 – Porte Avant (Door Contact, RDC)
  new SpanAccessory();
    new homeSpanIdentify("Porte Avant RDC", "DSC", "000000", "Contact", "3.0");
    new dscZone(1);

  // Zone 2 – Porte Arrière (Door Contact, SS)
  new SpanAccessory();
    new homeSpanIdentify("Porte Arrière SS", "DSC", "000000", "Contact", "3.0");
    new dscZone(2);

  // Zone 3 – Porte Patio RDC (Door Contact, RDC)
  new SpanAccessory();
    new homeSpanIdentify("Porte Patio RDC", "DSC", "000000", "Contact", "3.0");
    new dscZone(3);

  // Zone 4 – Porte Patio 2e (Door Contact, 2e)
  new SpanAccessory();
    new homeSpanIdentify("Porte Patio 2e", "DSC", "000000", "Contact", "3.0");
    new dscZone(4);

  // Zone 5 – Corridor (Motion, RDC)
  new SpanAccessory();
    new homeSpanIdentify("Corridor RDC", "DSC", "000000", "Motion", "3.0");
    new dscMotionZone(5);

  // Zone 6 – Salle à manger (Motion, RDC)
  new SpanAccessory();
    new homeSpanIdentify("Salle à manger RDC", "DSC", "000000", "Motion", "3.0");
    new dscMotionZone(6);

  // Zone 7 – Salon (Motion, RDC)
  new SpanAccessory();
    new homeSpanIdentify("Salon RDC", "DSC", "000000", "Motion", "3.0");
    new dscMotionZone(7);

  // Zone 8 – Bureau (Motion, 2e)
  new SpanAccessory();
    new homeSpanIdentify("Bureau 2e", "DSC", "000000", "Motion", "3.0");
    new dscMotionZone(8);

  // Zone 10 – Salle de bain principale (Leak Detector, 2e)
  new SpanAccessory();
    new homeSpanIdentify("Master Bathroom 2e", "DSC", "000000", "Leak", "3.0");
    new dscLeakZone(10);

  // Zone 11 – Détecteur de fumée (Fire zone, RDC) — triggers partition 1 fire flag
  new SpanAccessory();
    new homeSpanIdentify("Smoke Detector RDC", "DSC", "000000", "Smoke", "3.0");
    new dscFire(1);

  // Zone 12 – Fenêtre Ouest RDC (Window Contact, RDC)
  new SpanAccessory();
    new homeSpanIdentify("Fenêtre Ouest RDC", "DSC", "000000", "Contact", "3.0");
    new dscZone(12);

  // Zone 13 – Fenêtre Nord RDC (Window Contact, RDC)
  new SpanAccessory();
    new homeSpanIdentify("Fenêtre Nord RDC", "DSC", "000000", "Contact", "3.0");
    new dscZone(13);

  // Zone 17 – Salon (Motion, SS)
  new SpanAccessory();
    new homeSpanIdentify("Salon SS", "DSC", "000000", "Motion", "3.0");
    new dscMotionZone(17);

  // Zone 18 – Fenêtre Atelier (Window Contact, SS)
  new SpanAccessory();
    new homeSpanIdentify("Fenêtre Atelier SS", "DSC", "000000", "Contact", "3.0");
    new dscZone(18);

  // Zone 19 – Fenêtre Sud (Window Contact, SS)
  new SpanAccessory();
    new homeSpanIdentify("Fenêtre Sud SS", "DSC", "000000", "Contact", "3.0");
    new dscZone(19);

  // Zone 20 – Fenêtre Ouest SS (Window Contact, SS)
  new SpanAccessory();
    new homeSpanIdentify("Fenêtre Ouest SS", "DSC", "000000", "Contact", "3.0");
    new dscZone(20);

  // Zone 21 – Atelier (Motion, SS)
  new SpanAccessory();
    new homeSpanIdentify("Atelier SS", "DSC", "000000", "Motion", "3.0");
    new dscMotionZone(21);

  dsc.begin();

  new SpanUserCommand('T', "show current NTP and panel time", [](const char *) {
    tm t;
    if (getLocalTime(&t)) {
      char buf[32];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
      Serial.printf("NTP time:   %s\n", buf);
    } else {
      Serial.println("NTP time: not yet synced!");
    }
    if (ntpReady)
      Serial.printf("Panel time: %04d-%02d-%02d %02d:%02d\n",
                    dsc.year, dsc.month, dsc.day, dsc.hour, dsc.minute);
    else
      Serial.println("Panel time: unknown (no timestamp received yet)");
  });

  new SpanUserCommand('N', "force NTP time sync to panel", [](const char *) {
    tm t;
    if (!getLocalTime(&t)) {
      Serial.println("NTP force sync: NTP not yet synced, try again later");
      return;
    }
    ntpSynced    = false;
    ntpImmediate = true;
    Serial.println("NTP force sync: queued");
  });

  activityLog("info", "system", "Boot complete — firmware v3.0");
  activityFlush();
}


void loop() {
  // Log NTP status once on first sync, then every 60 s
  if (getLocalTime(&ntpTime)) {
    if (!ntpReady) {
      ntpReady = true;
      char buf[32];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ntpTime);
      LOG1("NTP synced: %s\n", buf);
      activityLog("info", "system", "NTP synced");
    } else if (millis() - ntpLastLog >= 60000) {
      ntpLastLog = millis();
      char buf[32];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ntpTime);
      LOG1("NTP: %s\n", buf);
    }
  }

  // Push NTP time to panel when needed
  if (!ntpSynced && getLocalTime(&ntpTime)) {
    if (ntpImmediate || ntpTime.tm_sec == ntpOffset) {
      if (dsc.ready[timePartition - 1] &&
          dsc.setTime(ntpTime.tm_year + 1900, ntpTime.tm_mon + 1,
                      ntpTime.tm_mday, ntpTime.tm_hour, ntpTime.tm_min,
                      accessCode, timePartition)) {
        ntpSynced    = true;
        ntpImmediate = false;
        LOG1("Panel time synchronized\n");
        activityLog("info", "system", "Panel time synchronized via NTP");
      }
    }
  }

  homeSpan.poll();
  dsc.loop();

  // Consume any arm/disarm command queued by the HTTP handler — keep on main task.
  // MUST be static: dsc.write(const char*) stores the pointer for async ISR use,
  // so the buffer must outlive this loop() call.
  static char pendingWrite[16];
  if (webApiConsumePendingWrite(pendingWrite, sizeof(pendingWrite))) {
    dsc.writePartition = 1;
    if (pendingWrite[1] == '\0')
      dsc.write(pendingWrite[0]);  // single-char commands: 's' (stay), 'w' (away)
    else
      dsc.write(pendingWrite);     // PIN string for disarm — pointer stays valid (static)
  }

  if (dsc.statusChanged) {
    dsc.statusChanged = false;

    if (dsc.keybusChanged) {
      dsc.keybusChanged = false;
      bool connected = dsc.keybusConnected;
      Serial.printf("[DSC] Keybus %s\n", connected ? "connected" : "disconnected");
      if (connected)
        activityLog("info", "system", "Keybus connected");
      else
        activityLog("warn", "system", "Keybus disconnected");
    }

    if (dsc.bufferOverflow) {
      Serial.println("[DSC] Keybus buffer overflow");
      dsc.bufferOverflow = false;
    }

    if (dsc.accessCodePrompt) {
      dsc.accessCodePrompt = false;
      dsc.write(accessCode);
    }

    for (byte partition = 0; partition < dscPartitions; partition++) {
      if (dsc.disabled[partition] || !configuredPartitions[partition]) continue;

      if (dsc.armedChanged[partition] || dsc.exitDelayChanged[partition] || dsc.alarmChanged[partition])
        updatePartitions = true;

      if (dsc.armedChanged[partition]) {
        const char *state =
            dsc.alarm[partition]       ? "alarm"       :
            dsc.entryDelay[partition]  ? "entry_delay" :
            (dsc.armed[partition] && dsc.noEntryDelay[partition]) ? "armed_night" :
            (dsc.armed[partition] && dsc.armedAway[partition])    ? "armed_away"  :
            (dsc.armed[partition] && dsc.armedStay[partition])    ? "armed_stay"  :
            dsc.exitDelay[partition]   ? "exit_delay"  : "disarmed";
        activityLog("info", "keybus", "Partition %d → %s", partition + 1, state);
      }

      if (dsc.alarmChanged[partition] && dsc.alarm[partition]) {
        activityLog("error", "keybus", "Alarm triggered on partition %d", partition + 1);
      }

      if (dsc.fireChanged[partition]) {
        updateSmokeSensors = true;
        if (dsc.fire[partition])
          activityLog("error", "keybus", "Fire alarm triggered");
        else
          activityLog("info", "keybus", "Fire alarm cleared");
      }
    }

    if (dsc.openZonesStatusChanged) {
      logZoneChanges();
      dsc.openZonesStatusChanged = false;
      updateZones = true;
    }

    if (dsc.pgmOutputsStatusChanged) {
      dsc.pgmOutputsStatusChanged = false;
      updatePGMs = true;
    }

    if (dsc.timestampChanged) {
      dsc.timestampChanged = false;

      bool panelUnset = (dsc.year < 2020 || dsc.month == 0 || dsc.day == 0);
      if (panelUnset) {
        LOG1("Panel clock unset — will push NTP time when ready\n");
        ntpSynced    = false;
        ntpImmediate = true;
      } else if (getLocalTime(&ntpTime)) {
        bool drifted = (dsc.year  != (unsigned int)(ntpTime.tm_year + 1900) ||
                        dsc.month != (byte)(ntpTime.tm_mon + 1)             ||
                        dsc.day   != (byte)ntpTime.tm_mday                  ||
                        dsc.hour  != (byte)ntpTime.tm_hour                  ||
                        dsc.minute!= (byte)ntpTime.tm_min);
        if (drifted) {
          ntpOffset    = ntpTime.tm_sec;
          ntpSynced    = false;
          ntpImmediate = false;
          LOG1("Panel clock drifted — scheduling correction\n");
        }
      }
    }

    wsPush();
  }

  unsigned long now = millis();

  // WebSocket heartbeat every 10 s
  if (now - sLastWsPushMs >= WS_PUSH_INTERVAL_MS) {
    sLastWsPushMs = now;
    wsPush();
  }

  // Periodic activity log flush every 30 s
  if (now - sLastFlushMs >= FLUSH_INTERVAL_MS) {
    sLastFlushMs = now;
    activityFlush();
  }
}
