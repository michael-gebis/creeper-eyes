// Dark panels at night.
//
// A prop in a hallway does not need to stare all night, so between two times
// the user sets, the panels go dark and the eyes stop being rendered.  Design
// notes, including what was measured and what was only assumed, are in
// docs/SLEEP.md.
//
// Three things worth knowing before reading the implementation.
//
// It goes dark using the panel's own command rather than by drawing black.
// An OLED showing black pixels is already dark, so a frame of black spends a
// full SPI push to achieve what one byte does, and leaves the panel driving
// its rows besides.  See displaySetPower() in display.h.
//
// It will not engage unless the board actually knows the time.  A head with
// no network, no RTC and no hand-set time must not decide it is three in the
// morning: it reports "waiting for the time" and stays awake.  The clock face
// already hides itself under exactly this condition, and for the same reason.
//
// And it is the lowest-priority claim on the panels, not the highest.
// Someone who asks for the address at 3am gets the address; an update landing
// overnight still says so.  Sleep sits below those in frame().

#ifndef SLEEPMODE_H
#define SLEEPMODE_H

#include "config.h"

#if SLEEP

#include <stdint.h>

// Load the stored window, or fall back to the built-in one.
void sleepBegin(void);

// Called once per frame, after the cards and the splash have had their say.
// Returns true when the panels are dark and there is nothing to draw, which
// is the caller's cue to skip rendering entirely.
bool sleepPoll(void);

// Someone did something deliberate -- a command, a button press, an API call
// that changes state.  Holds the eyes awake for SLEEP_WAKE_S even inside the
// window.  Deliberately *not* called by the page's polling, or a browser tab
// left open would keep the head awake all night.
void sleepNudge(void);

// Drop any hold a nudge is applying, so the window takes effect now.
// Changing the schedule does this: having just been told when to sleep, the
// board should not sit awake for another minute because telling it counted as
// someone being there.
void sleepCancelWake(void);

// Whether the window is currently in force.  False while a nudge is holding
// it awake, so this answers "are the eyes dark" rather than "is it night".
bool sleepIsAsleep(void);

// Why it is or is not asleep, for the page and the console.  One of:
// "off", "waiting for the time", "awake", "asleep", "woken".
const char *sleepReason(void);

// The settings.  Times are local minutes since midnight, 0-1439.
bool sleepEnabled(void);
void sleepSetEnabled(bool on);
uint16_t sleepStart(void);
uint16_t sleepStop(void);
bool sleepSetWindow(uint16_t startMin, uint16_t stopMin);
uint8_t sleepLevel(void);      // 0 = panels off, 1-100 = dimmed and still running
void sleepSetLevel(uint8_t percent);

// Minutes until the next transition, for a page that wants to say "asleep
// until 07:00".  Returns false when there is nothing to count towards.
bool sleepNextChange(uint16_t &minutesOut, bool &toAsleepOut);

// Persistence is handled by main.cpp's settings block along with everything
// else, so these expose the raw values for it to write and read back.
void sleepLoad(bool enabled, uint16_t start, uint16_t stop, uint8_t level);

#endif // SLEEP
#endif // SLEEPMODE_H
