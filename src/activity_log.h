#pragma once
#include <Arduino.h>

#define ACTIVITY_MAX_ENTRIES 200

struct ActivityEntry {
  char ts[24];
  char level[8];
  char source[12];
  char msg[128];
};

void   activityInit();
void   activityLog(const char *level, const char *source, const char *fmt, ...);
void   activityFlush();
String activitySerialize(int limit = 20);
