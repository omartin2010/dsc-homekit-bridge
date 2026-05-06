/*
 *  DSC-HomeKit-Ethernet
 *
 *  Adapted from dscKeybusInterface HomeKit-HomeSpan example for W5500 wired Ethernet.
 *  Original: https://github.com/taligentx/dscKeybusInterface/tree/master/examples/esp32/HomeKit-HomeSpan
 *
 *  Changes from the original:
 *    - WiFi disabled at startup to free RAM
 *    - W5500 SPI Ethernet initialised via ETH.begin() before homeSpan.begin()
 *    - dscClockPin moved 18 → 32, dscReadPin moved 19 → 33 to vacate the
 *      default VSPI pins (SCK=18, MISO=19) used by the W5500
 *
 *  W5500 SPI wiring (ESP32 → W5500 module):
 *    GPIO 18 (SCK)  → W5500 SCLK
 *    GPIO 19 (MISO) → W5500 MISO
 *    GPIO 22 (MOSI) → W5500 MOSI
 *    GPIO  5 (CS)   → W5500 SCS
 *    GPIO  4 (INT)  → W5500 INT   (leave W5500_IRQ_PIN as -1 if not wired)
 *    GPIO  0 (RST)  → W5500 RESET — GPIO 0 is LOW in download mode, keeping W5500 in reset during upload
 *
 *  DSC Keybus wiring (see AI.md):
 *    GPIO 32 (CLK)   ← DSC Yellow via resistor divider
 *    GPIO 33 (READ)  ← DSC Green  via resistor divider
 *    GPIO 21 (WRITE) → NPN base via 1kΩ, collector to DSC Green
 */

// DSC Classic series: uncomment for PC1500/PC1550 support
//#define dscClassicSeries

#include <WiFi.h>
#include <ETH.h>
#include <time.h>
#include "HomeSpan.h"
#include <dscKeybusInterface.h>

// Settings — access code lives in secrets.h (git-ignored)
#include "secrets.h"

// DSC Keybus pins  (18/19 are now used by W5500 SPI — moved to 32/33)
#define dscClockPin  32  // 4,13,16-39
#define dscReadPin   33  // 4,13,16-39
#define dscPC16Pin   17  // DSC Classic Series only
#define dscWritePin  21  // 4,13,16-33

// W5500 SPI pins
#define W5500_CS_PIN   5
#define W5500_IRQ_PIN  4   // Set to -1 if INT pin not connected
#define W5500_RST_PIN  0   // GPIO 0 = LOW during download mode → W5500 held in reset during upload
#define W5500_SCK_PIN  18
#define W5500_MISO_PIN 19
#define W5500_MOSI_PIN 22

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


void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();

  // Disable WiFi to free RAM — we use wired Ethernet only
  WiFi.mode(WIFI_OFF);

  // Initialise W5500 via SPI; HomeSpan auto-detects ETH and switches to Ethernet mode
  ETH.begin(ETH_PHY_W5500, /*phy_addr=*/1,
            W5500_CS_PIN, W5500_IRQ_PIN, W5500_RST_PIN,
            SPI3_HOST,
            W5500_SCK_PIN, W5500_MISO_PIN, W5500_MOSI_PIN);

  // Start SNTP — re-syncs automatically every hour; DHCP may not be up yet, SNTP retries
  configTzTime(timeZone, ntpServer);

  homeSpan.begin(Category::Bridges, "DSC Security System");

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

    if (dsc.bufferOverflow) {
      Serial.println(F("Keybus buffer overflow"));
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
