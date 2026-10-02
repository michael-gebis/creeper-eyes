// See dimmer.h.

#include "dimmer.h"

#include "config.h"
#include "display.h"
#include <Arduino.h>
#include <math.h>

static uint8_t setting = 100; // replaced by dimmerBegin() and dimmerLoad()
static uint8_t gammaX10 = DIM_GAMMA_X10;
static int8_t trim[2] = {0, 0};
static uint8_t sleepFactor = 100;
static bool sweeping = false;
static uint32_t sweepStartMs = 0;

// The level being shown, as a setting, moving towards target() at the fade
// rate.  A float, so a slow fade does not stall on rounding.
static float shown = 100.0f;
static uint32_t lastPollMs = 0;

// Setting to light output, as a fraction of the most the panel can drive.
static float luminance(float percent, uint8_t g) {
  if (percent <= 0.0f)
    return 0.0f;
  return powf(percent / 100.0f, g / 10.0f);
}

uint8_t dimmerDefaultPercent(void) {
  // The inverse of luminance(), at the default curve: whatever setting
  // lands exactly on the brightness the panels were initialised to.
  float p = 100.0f * powf(displayInitialLuminance(), 10.0f / DIM_GAMMA_X10);
  return (uint8_t)lroundf(p);
}

// Sends `shown` to the panels.  Off is the panel's own off command, which
// keeps what it is showing and stops driving its rows; anything above is the
// luminance for each panel, trimmed, with the brightness set before the power
// so a panel coming back on never flashes up at a stale level.
static void apply(void) {
  if (shown <= 0.0f) {
    displaySetPower(false);
    return;
  }
  const float lum = luminance(shown, gammaX10);
  for (uint8_t e = 0; e < displayCount(); e++) {
    float l = lum * (100 + trim[displayPosition(e)]) / 100.0f;
    displaySetLuminance(e, l > 1.0f ? 1.0f : l);
  }
  displaySetPower(true);
}

void dimmerBegin(void) {
  setting = dimmerDefaultPercent();
  shown = setting; // what the panels were initialised to; nothing to send
  lastPollMs = millis();
}

void dimmerLoad(uint8_t percent, uint8_t g, const int8_t t[2]) {
  setting = percent;
  gammaX10 = g;
  trim[0] = t[0];
  trim[1] = t[1];
}

uint8_t dimmerPercent(void) { return setting; }

void dimmerSetPercent(uint8_t percent) {
  setting = percent;
  sweeping = false; // asking for a level ends a sweep
}

uint8_t dimmerGammaX10(void) { return gammaX10; }

void dimmerSetGammaX10(uint8_t g) {
  gammaX10 = g;
  apply(); // same setting, new curve: seen at once, no fade to wait for
}

int8_t dimmerTrim(uint8_t position) {
  return position < 2 ? trim[position] : 0;
}

void dimmerSetTrim(uint8_t position, int8_t percent) {
  if (position >= 2)
    return;
  trim[position] = percent;
  apply();
}

void dimmerSetSweep(bool on) {
  sweeping = on;
  sweepStartMs = millis();
}

bool dimmerSweeping(void) { return sweeping; }

uint8_t dimmerShown(void) { return (uint8_t)lroundf(shown); }

void dimmerSetSleepFactor(uint8_t percent) { sleepFactor = percent; }

// 100 down to 1 and back up, DIM_SWEEP_MS each way.  Never 0: off would end
// the sweep in a blank panel rather than show the bottom of the curve.
static float sweepPercent(uint32_t now) {
  uint32_t phase = (now - sweepStartMs) % (2UL * DIM_SWEEP_MS);
  float t = (float)(phase % DIM_SWEEP_MS) / DIM_SWEEP_MS;
  return phase < DIM_SWEEP_MS ? 100.0f - 99.0f * t : 1.0f + 99.0f * t;
}

// Where the fade is heading.
static float target(uint32_t now) {
  float base = sweeping ? sweepPercent(now) : setting;
  return base * sleepFactor / 100.0f;
}

bool dimmerPoll(void) {
  const uint32_t now = millis();
  // Capped, so the first poll after a long card fades rather than jumps.
  uint32_t dt = now - lastPollMs;
  if (dt > 100)
    dt = 100;
  lastPollMs = now;

  const float want = target(now);
  if (shown != want) {
    const float step = 100.0f * dt / DIM_FADE_MS;
    shown = want > shown ? fminf(want, shown + step) : fmaxf(want, shown - step);
    apply();
  }
  return shown <= 0.0f;
}

void dimmerCard(void) {
  shown = setting ? setting : dimmerDefaultPercent();
  lastPollMs = millis();
  apply();
}
