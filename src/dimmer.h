// How bright the eyes are, and how they get there.  docs/BRIGHTNESS.md has
// the reasoning and the measurements.
//
// One setting, 0-100.  0 is off; anything else lights the panels, at a level
// that follows a gamma curve so that equal steps of the setting look like
// equal steps of brightness.  It is done in the panels' own current controls
// rather than by scaling pixels, which is why it saves power -- an OLED's
// draw follows the current through its pixels -- and why a dim eye keeps
// every shade it had: scaling pixels would take the greyscale panels' 16
// levels down to four at 25%.
//
// What the panels actually show is the setting, times sleep mode's level
// while asleep (60% and a sleep level of 50% is 30%), or a sweep while one
// runs.  Changes fade rather than jump.  A per-panel trim evens out two
// panels that do not match; like flip, it belongs to the panel, not the eye.
//
// This module decides when the panels are on at all.  Sleep mode feeds it a
// fraction instead of switching them itself, and cards -- the splash, the
// address cards, messages -- call dimmerCard() so they are readable even with
// the eyes turned down or off.
//
// Everything here runs in the render task.

#ifndef DIMMER_H
#define DIMMER_H

#include <stdint.h>

// Before anything is drawn.  The panels come up at the brightness their
// initialisation sets, and this starts from the setting that matches it.
void dimmerBegin(void);

// The saved values, from main.cpp's settings block.  Each already checked.
void dimmerLoad(uint8_t percent, uint8_t gammaX10, const int8_t trim[2]);

// The setting that reproduces the brightness the panels had before the
// dimmer existed, at the default gamma.  What a board with nothing saved
// starts at, so nothing changes until somebody asks for it.
uint8_t dimmerDefaultPercent(void);

uint8_t dimmerPercent(void);
void dimmerSetPercent(uint8_t percent); // 0-100; the caller checks the range

// The curve, in tenths: 22 is gamma 2.2.  Runtime, so curves can be compared
// without a rebuild.
uint8_t dimmerGammaX10(void);
void dimmerSetGammaX10(uint8_t gammaX10); // DIM_GAMMA_MIN..DIM_GAMMA_MAX

// Per panel, by chip-select slot -- 0 for SELECT_L_PIN, 1 for SELECT_R_PIN --
// as a percentage of the panel's brightness, -50 to +50.
int8_t dimmerTrim(uint8_t slot);
void dimmerSetTrim(uint8_t slot, int8_t percent);

// A slow run from full to nearly off and back, for judging a curve by eye.
// Replaces the setting while it runs; the setting itself is untouched.
void dimmerSetSweep(bool on);
bool dimmerSweeping(void);

// What the panels are showing this moment, 0-100, fades and sleep included.
uint8_t dimmerShown(void);

// Per frame, after the cards: sleep's fraction first (100 awake, its level
// asleep, 0 for off), then the poll, which moves the fade on.  The poll
// returns true once the panels have faded all the way off, which is the
// render loop's cue to draw nothing.
void dimmerSetSleepFactor(uint8_t percent);
bool dimmerPoll(void);

// A card is about to go up.  Lights the panels at once, at the setting -- or
// at the default if the setting is off -- ignoring sleep and sweeps: a card
// is shown because somebody needs to read it.  The next poll after the card
// fades back to wherever the eyes should be.
void dimmerCard(void);

#endif // DIMMER_H
