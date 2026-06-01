#include "activity_log.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <stdarg.h>
#include <time.h>

static ActivityEntry sEntries[ACTIVITY_MAX_ENTRIES];
static int  sHead  = 0;
static int  sCount = 0;
static bool sDirty = false;

static const char *ACTIVITY_FILE = "/activity.json";

static void getIsoTs(char *buf, size_t len) {
  time_t now = time(nullptr);
  if (now > 100000) {
    struct tm ti;
    localtime_r(&now, &ti);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%S", &ti);
  } else {
    strlcpy(buf, "1970-01-01T00:00:00", len);
  }
}

void activityInit() {
  sHead  = 0;
  sCount = 0;
  sDirty = false;

  if (!LittleFS.exists(ACTIVITY_FILE)) return;

  File f = LittleFS.open(ACTIVITY_FILE, "r");
  if (!f) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return;

  JsonArray arr = doc["entries"].as<JsonArray>();
  int loaded = 0;
  for (JsonObject e : arr) {
    if (loaded >= ACTIVITY_MAX_ENTRIES) break;
    ActivityEntry &ae = sEntries[loaded];
    strlcpy(ae.ts,     e["ts"]     | "1970-01-01T00:00:00", sizeof(ae.ts));
    strlcpy(ae.level,  e["level"]  | "info",                sizeof(ae.level));
    strlcpy(ae.source, e["source"] | "system",              sizeof(ae.source));
    strlcpy(ae.msg,    e["msg"]    | "",                    sizeof(ae.msg));
    loaded++;
  }

  // File is newest-first; reverse so ring buffer is oldest-first
  for (int i = 0; i < loaded / 2; i++) {
    ActivityEntry tmp       = sEntries[i];
    sEntries[i]             = sEntries[loaded - 1 - i];
    sEntries[loaded-1-i]    = tmp;
  }

  sHead  = loaded % ACTIVITY_MAX_ENTRIES;
  sCount = loaded;
  Serial.printf("[Activity] Loaded %d entries\n", loaded);
}

void activityLog(const char *level, const char *source, const char *fmt, ...) {
  ActivityEntry &ae = sEntries[sHead];
  getIsoTs(ae.ts, sizeof(ae.ts));
  strlcpy(ae.level,  level,  sizeof(ae.level));
  strlcpy(ae.source, source, sizeof(ae.source));

  va_list args;
  va_start(args, fmt);
  vsnprintf(ae.msg, sizeof(ae.msg), fmt, args);
  va_end(args);

  sHead = (sHead + 1) % ACTIVITY_MAX_ENTRIES;
  if (sCount < ACTIVITY_MAX_ENTRIES) sCount++;
  sDirty = true;

  Serial.printf("[Activity][%s] %s: %s\n", level, source, ae.msg);
}

void activityFlush() {
  if (!sDirty) return;

  JsonDocument doc;
  JsonArray arr = doc["entries"].to<JsonArray>();

  for (int i = 0; i < sCount; i++) {
    int idx = (sHead - 1 - i + ACTIVITY_MAX_ENTRIES) % ACTIVITY_MAX_ENTRIES;
    JsonObject e = arr.add<JsonObject>();
    e["ts"]     = sEntries[idx].ts;
    e["level"]  = sEntries[idx].level;
    e["source"] = sEntries[idx].source;
    e["msg"]    = sEntries[idx].msg;
  }

  File f = LittleFS.open(ACTIVITY_FILE, "w");
  if (f) {
    serializeJson(doc, f);
    f.close();
    sDirty = false;
  }
}

String activitySerialize(int limit) {
  if (limit <= 0) limit = 20;
  if (limit > ACTIVITY_MAX_ENTRIES) limit = ACTIVITY_MAX_ENTRIES;
  if (limit > sCount) limit = sCount;

  JsonDocument doc;
  JsonArray arr = doc["entries"].to<JsonArray>();

  for (int i = 0; i < limit; i++) {
    int idx = (sHead - 1 - i + ACTIVITY_MAX_ENTRIES) % ACTIVITY_MAX_ENTRIES;
    JsonObject e = arr.add<JsonObject>();
    e["ts"]     = sEntries[idx].ts;
    e["level"]  = sEntries[idx].level;
    e["source"] = sEntries[idx].source;
    e["msg"]    = sEntries[idx].msg;
  }

  String result;
  serializeJson(doc, result);
  return result;
}
