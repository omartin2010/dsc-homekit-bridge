#include "web_api.h"
#include "activity_log.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <AsyncWebSocket.h>
#include <Preferences.h>
#include <time.h>
#include "secrets.h"

#define CONFIG_PATH   "/config.json"

// DSC state access — buildStatusJson() is defined in main.cpp where
// dscKeybusInterface.h is included. We forward-declare it here to avoid
// including that header in a second translation unit (its static member
// definitions cause multiply-defined symbol errors at link time).
extern String buildStatusJson();

// For the arm/disarm endpoint we only need to know if the panel is ready
// and armed. These thin accessors avoid re-including dscKeybusInterface.h.
extern bool dscReady();
extern bool dscArmed();
extern bool dscExitDelay();
extern bool dscAlarm();

static AsyncWebServer sServer(80);
static AsyncWebSocket sWs("/ws");

static char              sPendingWrite[16] = "";
static bool              sPendingWriteFlag = false;
static SemaphoreHandle_t sPendingWriteMutex = nullptr;

// Effective PIN — secrets.h value at startup, overridden by panel.access_code in config.json.
static char sEffectivePin[16];

extern bool gLogMotion;
extern bool gBypassedZones[33];

static void loadEffectivePin() {
  strlcpy(sEffectivePin, accessCode, sizeof(sEffectivePin));
  gLogMotion = false;
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return;
  JsonDocument doc;
  if (!deserializeJson(doc, f)) {
    const char *code = doc["panel"]["access_code"] | "";
    if (code && strlen(code) >= 4)
      strlcpy(sEffectivePin, code, sizeof(sEffectivePin));
    gLogMotion = doc["panel"]["log_motion"] | false;
  }
  f.close();
}

// ── Helpers ───────────────────────────────────────────────────────────────────

static String isoTs() {
  time_t now = time(nullptr);
  char buf[20];
  if (now > 100000) {
    struct tm ti;
    localtime_r(&now, &ti);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &ti);
  } else {
    strlcpy(buf, "1970-01-01T00:00:00", sizeof(buf));
  }
  return String(buf);
}

static void addCors(AsyncWebServerResponse *r) {
  r->addHeader("Access-Control-Allow-Origin",  "*");
  r->addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  r->addHeader("Access-Control-Allow-Headers", "Content-Type");
}

static void sendJson(AsyncWebServerRequest *req, int code, const String &json) {
  AsyncWebServerResponse *r = req->beginResponse(code, "application/json", json);
  addCors(r);
  req->send(r);
}

static void sendError(AsyncWebServerRequest *req, int code, const char *msg) {
  JsonDocument doc;
  doc["ok"]    = false;
  doc["error"] = msg;
  String out;
  serializeJson(doc, out);
  sendJson(req, code, out);
}

static void handleOptions(AsyncWebServerRequest *req) {
  AsyncWebServerResponse *r = req->beginResponse(204);
  addCors(r);
  req->send(r);
}

// ── WebSocket push ────────────────────────────────────────────────────────────

void wsPush() {
  sWs.cleanupClients();
  if (sWs.count() == 0) return;
  sWs.textAll(buildStatusJson());
}

// ── Pending write queue ───────────────────────────────────────────────────────

bool webApiConsumePendingWrite(char *out, size_t len) {
  if (!sPendingWriteFlag || !sPendingWriteMutex) return false;
  if (xSemaphoreTake(sPendingWriteMutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;
  bool had = sPendingWriteFlag;
  if (had) {
    strlcpy(out, sPendingWrite, len);
    sPendingWriteFlag = false;
  }
  xSemaphoreGive(sPendingWriteMutex);
  return had;
}

static void queueDscWrite(const char *cmd) {
  if (!sPendingWriteMutex) return;
  if (xSemaphoreTake(sPendingWriteMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
  strlcpy(sPendingWrite, cmd, sizeof(sPendingWrite));
  sPendingWriteFlag = true;
  xSemaphoreGive(sPendingWriteMutex);
}

// ── Route handlers ────────────────────────────────────────────────────────────

static void handleStatus(AsyncWebServerRequest *req) {
  sendJson(req, 200, buildStatusJson());
}

static void handleGetConfig(AsyncWebServerRequest *req) {
  JsonDocument cfg;
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (f) {
    deserializeJson(cfg, f);
    f.close();
  } else {
    cfg["rules"].to<JsonArray>();
    JsonObject panel  = cfg["panel"].to<JsonObject>();
    panel["auto_arm_away"]  = false;
    panel["entry_delay_s"]  = 30;
    panel["exit_delay_s"]   = 45;
    panel["access_code"]    = "";
    panel["log_motion"]     = false;
    panel["chime_enabled"]  = true;
  }
  String out;
  serializeJson(cfg, out);
  sendJson(req, 200, out);
}

static void handlePostConfig(AsyncWebServerRequest *req, uint8_t *data,
                              size_t len, size_t /*index*/, size_t /*total*/) {
  String body;
  body.reserve(len);
  for (size_t i = 0; i < len; i++) body += (char)data[i];

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    sendError(req, 400, "Invalid JSON body");
    return;
  }

  if (doc["rules"].is<JsonArray>()) {
    for (JsonObject rule : doc["rules"].as<JsonArray>()) {
      const char *action = rule["action"] | "";
      if (strcmp(action, "log_info")  != 0 &&
          strcmp(action, "log_warn")  != 0 &&
          strcmp(action, "log_error") != 0) {
        sendError(req, 422, "action must be log_info, log_warn, or log_error");
        return;
      }
    }
  }

  if (doc["panel"].is<JsonObject>()) {
    const char *code = doc["panel"]["access_code"] | "";
    if (code && *code) {
      size_t clen = strlen(code);
      if (clen < 4 || clen > 8) {
        sendError(req, 422, "panel.access_code must be 4–8 digits");
        return;
      }
      for (size_t i = 0; i < clen; i++) {
        if (code[i] < '0' || code[i] > '9') {
          sendError(req, 422, "panel.access_code must contain only digits");
          return;
        }
      }
    }
  }

  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) { sendError(req, 500, "LittleFS write failed"); return; }
  serializeJson(doc, f);
  f.close();

  loadEffectivePin();
  activityLog("info", "api", "config updated via POST /api/config");

  JsonDocument resp;
  resp["ok"]       = true;
  resp["saved_at"] = isoTs();
  String out;
  serializeJson(resp, out);
  sendJson(req, 200, out);
}

static void handleGetActivity(AsyncWebServerRequest *req) {
  int limit = 20;
  if (req->hasParam("limit")) {
    limit = req->getParam("limit")->value().toInt();
    if (limit < 1)   limit = 1;
    if (limit > 100) limit = 100;
  }
  sendJson(req, 200, activitySerialize(limit));
}

static void handleCommand(AsyncWebServerRequest *req, uint8_t *data,
                          size_t len, size_t /*index*/, size_t /*total*/) {
  String body;
  body.reserve(len);
  for (size_t i = 0; i < len; i++) body += (char)data[i];

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    sendError(req, 400, "Invalid JSON body");
    return;
  }

  const char *cmd = doc["cmd"] | "";
  const char *pin = doc["pin"] | "";

  if (!cmd || !*cmd) { sendError(req, 400, "cmd required"); return; }
  if (!pin || !*pin) { sendError(req, 400, "pin required"); return; }

  if (strcmp(pin, sEffectivePin) != 0) {
    activityLog("warn", "api", "arm/disarm command rejected — wrong PIN");
    sendError(req, 403, "Unauthorized");
    return;
  }

  if (strcmp(cmd, "arm_stay") == 0) {
    if (!dscReady()) {
      activityLog("warn", "api", "arm_stay rejected — panel not ready");
      sendError(req, 422, "Panel not ready");
      return;
    }
    queueDscWrite("s");
    activityLog("info", "api", "arm stay queued via API");
  } else if (strcmp(cmd, "arm_away") == 0) {
    if (!dscReady()) {
      activityLog("warn", "api", "arm_away rejected — panel not ready");
      sendError(req, 422, "Panel not ready");
      return;
    }
    queueDscWrite("w");
    activityLog("info", "api", "arm away queued via API");
  } else if (strcmp(cmd, "disarm") == 0) {
    if (!dscArmed() && !dscExitDelay() && !dscAlarm()) {
      sendError(req, 422, "Panel is already disarmed");
      return;
    }
    queueDscWrite(sEffectivePin);
    activityLog("info", "api", "disarm queued via API");
  } else if (strcmp(cmd, "arm_night") == 0) {
    if (!dscReady()) {
      activityLog("warn", "api", "arm_night rejected — panel not ready");
      sendError(req, 422, "Panel not ready");
      return;
    }
    queueDscWrite("n");
    activityLog("info", "api", "arm night queued via API");
  } else if (strcmp(cmd, "alarm_reset") == 0) {
    queueDscWrite("r");
    activityLog("info", "api", "alarm reset queued via API");
  } else if (strcmp(cmd, "chime_toggle") == 0) {
    queueDscWrite("c");
    activityLog("info", "api", "chime toggled via API");
  } else if (strcmp(cmd, "bypass") == 0) {
    int zone = doc["zone"] | 0;
    if (zone < 1 || zone > 32) {
      sendError(req, 422, "zone must be 1-32");
      return;
    }
    if (dscArmed()) {
      sendError(req, 422, "Cannot bypass while armed");
      return;
    }
    char seq[8];
    snprintf(seq, sizeof(seq), "*1%02d#", zone);
    queueDscWrite(seq);
    gBypassedZones[zone] = !gBypassedZones[zone];
    activityLog("info", "api", "zone %d %s via API", zone,
                gBypassedZones[zone] ? "bypassed" : "unbypassed");
    JsonDocument resp;
    resp["ok"]       = true;
    resp["bypassed"] = gBypassedZones[zone];
    String out;
    serializeJson(resp, out);
    sendJson(req, 200, out);
    return;
  } else {
    sendError(req, 400, "unknown cmd");
    return;
  }

  JsonDocument resp;
  resp["ok"] = true;
  String out;
  serializeJson(resp, out);
  sendJson(req, 200, out);
}

static void handleHomekitReset(AsyncWebServerRequest *req) {
  activityLog("warn", "system", "HomeKit pairing reset via web API");
  Preferences prefs;
  prefs.begin("HPAIR", false); prefs.clear(); prefs.end();
  prefs.begin("HAP",   false); prefs.clear(); prefs.end();
  sendJson(req, 200, "{\"ok\":true,\"msg\":\"HomeKit pairing cleared — rebooting\"}");
  delay(500);
  ESP.restart();
}

static void handleReboot(AsyncWebServerRequest *req) {
  sendJson(req, 200, "{\"ok\":true,\"msg\":\"Rebooting\"}");
  delay(500);
  ESP.restart();
}

// ── WebSocket event handler ───────────────────────────────────────────────────

static void onWsEvent(AsyncWebSocket * /*server*/, AsyncWebSocketClient *client,
                      AwsEventType type, void * /*arg*/, uint8_t * /*data*/, size_t /*len*/) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("[WS] Client #%u connected\n", client->id());
      client->text(buildStatusJson());
      break;
    case WS_EVT_DISCONNECT:
      Serial.printf("[WS] Client #%u disconnected\n", client->id());
      sWs.cleanupClients();
      break;
    case WS_EVT_ERROR:
      Serial.printf("[WS] Client #%u error\n", client->id());
      sWs.cleanupClients();
      break;
    default:
      break;
  }
}

// ── Setup ─────────────────────────────────────────────────────────────────────

void setupWebServer() {
  sPendingWriteMutex = xSemaphoreCreateMutex();
  loadEffectivePin();

  sServer.on("/api/status",         HTTP_GET,  handleStatus);
  sServer.on("/api/config",         HTTP_GET,  handleGetConfig);
  sServer.on("/api/activity",       HTTP_GET,  handleGetActivity);
  sServer.on("/api/homekit/reset",  HTTP_POST, handleHomekitReset);
  sServer.on("/api/reboot",         HTTP_POST, handleReboot);

  sServer.on("/api/config", HTTP_POST,
    [](AsyncWebServerRequest *req) {},
    nullptr,
    handlePostConfig
  );
  sServer.on("/api/command", HTTP_POST,
    [](AsyncWebServerRequest *req) {},
    nullptr,
    handleCommand
  );

  sWs.onEvent(onWsEvent);
  sServer.addHandler(&sWs);

  sServer.onNotFound([](AsyncWebServerRequest *req) {
    if (req->method() == HTTP_OPTIONS) { handleOptions(req); return; }
    sendError(req, 404, "Not found");
  });

  sServer.begin();
  Serial.println("[Web] HTTP server started on port 80 (WebSocket: /ws)");
}
