// Wall-clock time and the timezone, independent of where the time came from.
//
// This used to live in net.cpp, which meant a no-network build had no concept
// of a timezone at all.  That was fine while NTP was the only source of real
// time; it stopped being fine when a battery-backed RTC became a second one,
// because an offline board still has to know that its stored UTC is five
// hours behind Chicago, and still has to get DST right in March.
//
// So: this module owns the timezone and the "do we know what time it is"
// flag, and is compiled into every build.  net.cpp and rtc.cpp are sources
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

// Adopt a timezone by name or POSIX string.  False if it is too long to
// store; the caller decides what to say about that.
bool timeSetTz(const char *nameOrPosix);

// Hand the system clock a UTC epoch.  Later sources outrank earlier ones --
// NTP overrules a restored RTC, and both overrule the free-running counter --
// so a late NTP answer is never undone by anything.
void timeAccept(time_t utc, TimeSource from);

// Where the time currently on show came from.
TimeSource timeSource(void);
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
