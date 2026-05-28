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
#include <time.h>
#include "HomeSpan.h"
#include <dscKeybusInterface.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

// Settings — access code lives in secrets.h (git-ignored)
#include "secrets.h"

static char gHomekitCode[16] = "46637726";

// ── Log ring buffer ───────────────────────────────────────────────────────────
#define LOG_LINES 48
#define LOG_WIDTH 128

static char              sLog[LOG_LINES][LOG_WIDTH];
static uint32_t          sLogMs[LOG_LINES];
static int               sLogHead  = 0;
static int               sLogCount = 0;
static SemaphoreHandle_t sLogMutex = nullptr;

static void logLine(const char *fmt, ...) {
  char tmp[LOG_WIDTH];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  Serial.print(tmp);
  if (!sLogMutex) return;
  xSemaphoreTake(sLogMutex, portMAX_DELAY);
  sLogMs[sLogHead] = millis();
  strlcpy(sLog[sLogHead], tmp, LOG_WIDTH);
  sLogHead = (sLogHead + 1) % LOG_LINES;
  if (sLogCount < LOG_LINES) sLogCount++;
  xSemaphoreGive(sLogMutex);
}

// DSC Keybus pins
#define dscClockPin  32  // 4,13,16-39
#define dscReadPin   33  // 4,13,16-39
#define dscPC16Pin   16  // DSC Classic Series only (GPIO17 reserved for ETH clock)
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
bool ntpSynced    = true;   // false = need to push time to panel
bool ntpImmediate = false;  // true = panel clock was unset, push ASAP without offset trick
byte ntpOffset    = 0;      // NTP second value at which to push (minimises sub-minute error)
tm   ntpTime;
bool ntpReady     = false;  // true once SNTP has delivered a first timestamp
unsigned long ntpLastLog = 0;

#include "dscHomeSpanAccessories.h"

static AsyncWebServer sWebServer(80);

static void onEthEvent(arduino_event_id_t event, arduino_event_info_t) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      logLine("[ETH] Started\n");
      ETH.setHostname("dsc");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:    logLine("[ETH] Link up\n");          break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      logLine("[ETH] IP: %s\n", ETH.localIP().toString().c_str());
      MDNS.addService("http", "tcp", 80);
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:     logLine("[ETH] Lost IP\n");           break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:logLine("[ETH] Link down\n");         break;
    case ARDUINO_EVENT_ETH_STOP:        logLine("[ETH] Stopped\n");           break;
    default: break;
  }
}

static void startWebServer() {
  sWebServer.on("/api/log", HTTP_GET, [](AsyncWebServerRequest *req) {
    String out = "[";
    if (xSemaphoreTake(sLogMutex, pdMS_TO_TICKS(50))) {
      int start = (sLogCount < LOG_LINES) ? 0 : sLogHead;
      int cnt   = (sLogCount < LOG_LINES) ? sLogCount : LOG_LINES;
      for (int i = 0; i < cnt; i++) {
        int idx = (start + i) % LOG_LINES;
        if (i) out += ',';
        out += "{\"t\":";
        out += sLogMs[idx];
        out += ",\"msg\":\"";
        for (const char *p = sLog[idx]; *p; p++) {
          if      (*p == '"')  out += "\\\"";
          else if (*p == '\\') out += "\\\\";
          else if (*p == '\n') out += "\\n";
          else if (*p != '\r') out += *p;
        }
        out += "\"}";
      }
      xSemaphoreGive(sLogMutex);
    }
    out += "]";
    req->send(200, "application/json", out);
  });

  sWebServer.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", "{\"ok\":true,\"msg\":\"Rebooting\"}");
    delay(500);
    ESP.restart();
  });

  sWebServer.on("/api/homekit/reset", HTTP_POST, [](AsyncWebServerRequest *req) {
    Preferences prefs;
    prefs.begin("HPAIR", false); prefs.clear(); prefs.end();
    prefs.begin("HAP",   false); prefs.clear(); prefs.end();
    req->send(200, "application/json",
              "{\"ok\":true,\"msg\":\"HomeKit pairing cleared — rebooting\"}");
    delay(500);
    ESP.restart();
  });
  sWebServer.onNotFound([](AsyncWebServerRequest *req) {
    req->send(404, "application/json", "{\"ok\":false,\"error\":\"Not found\"}");
  });
  sWebServer.begin();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  sLogMutex = xSemaphoreCreateMutex();

  WiFi.onEvent(onEthEvent);

  // Initialise onboard LAN8720A via RMII; HomeSpan auto-detects ETH and switches to Ethernet mode
  ETH.begin(ETH_PHY_LAN8720, /*phy_addr=*/0, /*mdc=*/23, /*mdio=*/18, /*rst=*/5, ETH_CLOCK_GPIO17_OUT);


  // Start SNTP — re-syncs automatically every hour; DHCP may not be up yet, SNTP retries
  configTzTime(timeZone, ntpServer);

  homeSpan.setPortNum(8080);
  homeSpan.enableOTA(false);
  homeSpan.setPairingCode(gHomekitCode);
  homeSpan.begin(Category::Bridges, "DSC Security System", "dsc");
  homeSpan.setHostNameSuffix("");

  startWebServer();

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
      Serial.println("NTP time: not yet synced! Should be soon.");
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
}


void loop() {
  // Log NTP status once on first sync, then every 60 s (visible at log level >= 1)
  if (getLocalTime(&ntpTime)) {
    if (!ntpReady) {
      ntpReady = true;
      char buf[32];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ntpTime);
      LOG1("NTP synced: %s\n", buf);
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
      }
    }
  }

  homeSpan.poll();
  dsc.loop();

  if (dsc.statusChanged) {
    dsc.statusChanged = false;

    if (dsc.keybusChanged) {
      dsc.keybusChanged = false;
      logLine("[DSC] Keybus %s\n", dsc.keybusConnected ? "connected" : "disconnected");
    }

    if (dsc.bufferOverflow) {
      logLine("[DSC] Keybus buffer overflow\n");
      dsc.bufferOverflow = false;
    }

    if (dsc.accessCodePrompt) {
      dsc.accessCodePrompt = false;
      dsc.write(accessCode);
    }

    for (byte partition = 0; partition < dscPartitions; partition++) {
      if (dsc.disabled[partition]) continue;

      if (dsc.armedChanged[partition] || dsc.exitDelayChanged[partition] || dsc.alarmChanged[partition])
        updatePartitions = true;

      if (dsc.fireChanged[partition])
        updateSmokeSensors = true;
    }

    if (dsc.openZonesStatusChanged) {
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
  }
}
