// See sleepmode.h.

#include "sleepmode.h"

#if SLEEP

#include "timekeeping.h"
#include <Arduino.h>

static bool enabled = SLEEP_ENABLED;
static uint16_t startMin = SLEEP_START_MIN;
static uint16_t stopMin = SLEEP_STOP_MIN;
static uint8_t level = SLEEP_LEVEL;

// While this is in the future the eyes stay up, window or no window.
static uint32_t wakeUntil = 0;

void sleepLoad(bool en, uint16_t start, uint16_t stop, uint8_t lvl) {
  enabled = en;
  if (start < 1440 && stop < 1440) {
    startMin = start;
    stopMin = stop;
  }
  level = lvl > 100 ? 100 : lvl;
}

bool sleepEnabled(void) { return enabled; }
uint16_t sleepStart(void) { return startMin; }
uint16_t sleepStop(void) { return stopMin; }
uint8_t sleepLevel(void) { return level; }
// Defined below; decide() needs them before they appear.
static bool inWindow(uint16_t now, uint16_t start, uint16_t stop);
static bool nowMinutes(uint16_t &out);

// The decision, as a pure function of the settings, the clock and the hold.
//
// Separate from sleepPoll() because the answer has to be available the
// instant it changes, not on the next frame.  A value cached by the poll
// would make every reply to PUT /sleep carry the *previous* window's
// conclusion -- a one-step lag that makes a correct implementation look
// broken to anyone driving the API.
//
// Deciding is pure; only sleepPoll() acts on it.
static bool decide(const char **why) {
  if (!enabled) {
    *why = "off";
    return false;
  }
  uint16_t now;
  if (!nowMinutes(now)) {
    // The guard that matters: no clock, no sleeping.
    *why = "waiting for the time";
    return false;
  }
  if (!inWindow(now, startMin, stopMin)) {
    *why = "awake";
    return false;
  }
  if (wakeUntil && (int32_t)(millis() - wakeUntil) < 0) {
    *why = "woken";
    return false;
  }
  *why = "asleep";
  return true;
}

bool sleepIsAsleep(void) {
  const char *why;
  return decide(&why);
}

const char *sleepReason(void) {
  const char *why = "off";
  decide(&why);
  return why;
}

void sleepSetEnabled(bool on) {
  enabled = on;
  if (!on)
    wakeUntil = 0;
}

bool sleepSetWindow(uint16_t start, uint16_t stop) {
  if (start >= 1440 || stop >= 1440)
    return false;
  startMin = start;
  stopMin = stop;
  return true;
}

void sleepSetLevel(uint8_t percent) { level = percent > 100 ? 100 : percent; }

void sleepCancelWake(void) { wakeUntil = 0; }

void sleepNudge(void) {
#if SLEEP_WAKE_S
  wakeUntil = millis() + (uint32_t)SLEEP_WAKE_S * 1000UL;
  if (!wakeUntil)
    wakeUntil = 1; // millis() wrapped to exactly 0; 0 means "not holding"
#endif
}

// Is `now` inside [start, stop)?
//
// Both forms are written out because the one that matters here is the second:
// a sleep window almost always runs across midnight, and the obvious
// comparison is the one that only handles the case nobody uses.
static bool inWindow(uint16_t now, uint16_t start, uint16_t stop) {
  if (start == stop)
    return false; // a zero-length window means never -- see docs/SLEEP.md
  if (start < stop)
    return now >= start && now < stop; // 01:00 -> 06:00
  return now >= start || now < stop;   // 22:00 -> 07:00
}

// Local minutes since midnight, or false when the time is not known.
static bool nowMinutes(uint16_t &out) {
  uint32_t sec;
  if (!timeLocalSecOfDay(sec))
    return false;
  out = (uint16_t)((sec / 60) % 1440);
  return true;
}

bool sleepNextChange(uint16_t &minutesOut, bool &toAsleepOut) {
  uint16_t now;
  if (!enabled || startMin == stopMin || !nowMinutes(now))
    return false;
  uint16_t target = inWindow(now, startMin, stopMin) ? stopMin : startMin;
  minutesOut = (uint16_t)((target + 1440 - now) % 1440);
  toAsleepOut = (target == startMin);
  return true;
}

uint8_t sleepPoll(void) {
  const char *why;
  bool want = decide(&why);
  if (want)
    wakeUntil = 0; // the hold has expired; stop carrying it

  // The conclusion as a fraction of the brightness setting, every poll
  // rather than only on the asleep/awake edge, or changing the level while
  // already asleep would never reach the panels.  The dimmer does the rest --
  // fading, switching
  // the panels off at 0, and telling the renderer when there is nothing to
  // draw.  A dimmed panel keeps drawing, so the eyes still move faintly.
  return want ? level : 100;
}

#endif // SLEEP
