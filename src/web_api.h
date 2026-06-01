#pragma once
#include <stddef.h>

void setupWebServer();
void wsPush();

// Consume a pending dsc.write() command queued by an HTTP handler.
// Returns true and fills out[] if a command is waiting; false otherwise.
// Call from loop() only — keeps dsc.write() on the main task.
bool webApiConsumePendingWrite(char *out, size_t len);
