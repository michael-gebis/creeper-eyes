// Wall-clock time and the timezone, independent of where the time came from.
//
// Compiled into every build, network or not: an offline board with an RTC
// still has to know that its stored UTC is five hours behind Chicago, and
// still has to get DST right in March.  This module owns the timezone and the
// "do we know what time it is" flag.  net.cpp and rtc.cpp are sources
// that feed it; neither is required, and with both compiled out the clock
// free-runs exactly as it always did.
//
// The system clock is the single place the time is kept.  A source calls
// timeAccept() with a UTC epoch, and everything downstream reads it back
// through localtime_r, which applies the timezone and its DST rules for
// free.  There is no second copy to drift.

#ifndef TIMEKEEPING_H
#define TIMEKEEPING_H

#include "config.h"

#include <Print.h>
#include <stdint.h>
#include <time.h>

// Where the current time came from.  Reported over the console and the API,
// because "the clock says 3:47" is much less useful than knowing whether
// that came off a time server, out of a battery-backed chip, or from a
// counter that started at 10:10 when the board booted.
//
// Declared in rank order, lowest first: timeAccept() refuses a source that
// ranks below the one in charge, so reordering this list changes which
// source wins.
enum TimeSource {
  TIME_FREE = 0, // free-running since boot; nothing has said what time it is
  TIME_RTC,      // restored from the RTC at boot
  TIME_MANUAL,   // somebody typed it: `clock set`
  TIME_NTP,      // a time server answered
};

// True once any source has supplied a real time.  The clock free-runs until
// it has.  (Named for what it means to a caller, not for NTP specifically --
// an RTC restore sets it just as surely.)
extern bool timeSynced;
extern char tzString[TZ_MAX];

// Named timezones, so nobody has to type a POSIX string from memory.  Named
// after cities the way the IANA database is, and grouped by region only so a
// picker can offer sixty of them without being a wall of text.
struct TzChoice {
  const char *name;
  const char *region;
  const char *posix;
};
extern const TzChoice tzChoices[];
extern const uint8_t numTzChoices;

// Returns the POSIX string for a name, or NULL if unknown.  Accepts the
// older regional names (`pacific`, `eastern`, ...) as well as the city ones.
const char *tzLookup(const char *name);

// Push tzString into the C library.  Call once at boot and again whenever the
// zone changes; localtime_r picks it up immediately, with no need to re-fetch
// the time from anywhere.
void timeApplyTz(void);

// Whether a string could be a POSIX TZ value: short enough to store, made
// of the characters that grammar uses, starting with a zone name and
// carrying an offset.  Plausible rather than parsed -- the C library does
// the parsing, and falls back to UTC on what it cannot read -- but enough to
// keep garbage, from a request or from flash, out of the environment.
bool timeTzValid(const char *posix);

// Adopt a timezone by name or POSIX string.  False if it is neither a known
// name nor a plausible POSIX string; the caller decides what to say.
bool timeSetTz(const char *nameOrPosix);

// Hand the system clock a UTC epoch.  Later sources outrank earlier ones --
// NTP overrules a restored RTC, and both overrule the free-running counter --
// so a late NTP answer is never undone by anything.
void timeAccept(time_t utc, TimeSource from);

// Give up a source's claim without changing the time it supplied.
//
// For when the thing that set the clock is switched off: what it gave us is
// still the best time we have and stays on the face, but it no longer
// outranks anybody.  Without this, turning NTP off would leave the clock
// defended by a server that is no longer being asked, and setting the time by
// hand afterwards would silently do nothing.
void timeRelinquish(TimeSource from);

// Whether the time came from something outside this board -- a time server
// or the RTC -- as opposed to being typed in or free-running.  `clock rate`
// only means anything when it is not, because the whole point of an external
// source is that it is not ours to speed up.
bool timeIsExternal(void);

// Where the time currently on show came from.
const char *timeSourceName(void);

// Local time now.  False while the clock is still free-running, in which case
// out is untouched.
bool timeLocal(struct tm &out);

// Seconds since midnight, local.  This is what the clock face wants: the
// eyes show no date, so nothing downstream needs one.
bool timeLocalSecOfDay(uint32_t &out);

// One line for the console.
void timeReport(Print &out);

#endif // TIMEKEEPING_H
