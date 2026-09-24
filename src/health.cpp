// See health.h.

#include "health.h"

#include "config.h"
#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>

static char lines[HEALTH_MAX][HEALTH_LINE];
static uint8_t kept = 0;
static uint8_t dropped = 0;

void healthNote(const char *fmt, ...) {
  char buf[HEALTH_LINE];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap); // truncates; always terminated
  va_end(ap);
  DEBUG_PRINTF("[health] %s\n", buf);
  if (kept < HEALTH_MAX) {
    memcpy(lines[kept++], buf, sizeof(buf));
  } else if (dropped < 255) {
    dropped++;
  }
}

uint8_t healthCount(void) { return kept; }

uint8_t healthDropped(void) { return dropped; }

const char *healthLine(uint8_t i) { return i < kept ? lines[i] : NULL; }
