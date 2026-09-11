// Wall-clock time and the timezone.  See timekeeping.h for why this is not
// part of net.cpp any more.

#include "timekeeping.h"

#include <string.h>
#include <sys/time.h>

#if RTC
#include "rtc.h"
#endif

bool timeSynced = false;
char tzString[TZ_MAX] = TZ_DEFAULT;

// What to report, and what it takes to replace it.  Normally the same thing;
// they come apart when a source is switched off -- see timeRelinquish.
static TimeSource source = TIME_FREE;
static TimeSource rank = TIME_FREE;

// Enough of the world to cover wherever the head ends up.  These are POSIX
// TZ strings, not the IANA database -- the database is megabytes and needs a
// filesystem, while a POSIX string is thirty bytes and is what the C library
// wants anyway.  The trade is that a country changing its DST rules needs a
// firmware update, which for a Halloween prop is the right side of the deal.
// Anything not listed can still be set: `tz` takes a raw POSIX string.
//
// Offsets are inverted relative to how people say them: UTC+2 is written -2.
// Zones without DST are a single field.
const TzChoice tzChoices[] = {
    // North America
    {"los_angeles", "North America", "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"denver", "North America", "MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"phoenix", "North America", "MST7"},
    {"chicago", "North America", "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"new_york", "North America", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"halifax", "North America", "AST4ADT,M3.2.0/2,M11.1.0/2"},
    {"st_johns", "North America", "NST3:30NDT,M3.2.0/2,M11.1.0/2"},
    {"anchorage", "North America", "AKST9AKDT,M3.2.0/2,M11.1.0/2"},
    {"honolulu", "North America", "HST10"},
    {"mexico_city", "North America", "CST6"}, // DST abolished in 2022
    {"panama", "North America", "EST5"},

    // South America
    {"bogota", "South America", "<-05>5"},
    {"lima", "South America", "<-05>5"},
    {"caracas", "South America", "<-04>4"},
    {"santiago", "South America", "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
    {"sao_paulo", "South America", "<-03>3"}, // DST abolished in 2019
    {"buenos_aires", "South America", "<-03>3"},

    // Europe
    {"reykjavik", "Europe", "GMT0"},
    {"london", "Europe", "GMT0BST,M3.5.0/1,M10.5.0/2"},
    {"dublin", "Europe", "GMT0IST,M3.5.0/1,M10.5.0/2"},
    {"lisbon", "Europe", "WET0WEST,M3.5.0/1,M10.5.0/2"},
    {"madrid", "Europe", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"paris", "Europe", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"berlin", "Europe", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"rome", "Europe", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"warsaw", "Europe", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"athens", "Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"helsinki", "Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"kyiv", "Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"moscow", "Europe", "MSK-3"},

    // Africa and the Middle East
    {"casablanca", "Africa / Middle East", "<+01>-1"},
    {"lagos", "Africa / Middle East", "WAT-1"},
    {"cairo", "Africa / Middle East", "EET-2EEST,M4.5.5/0,M10.5.4/24"},
    {"johannesburg", "Africa / Middle East", "SAST-2"},
    {"jerusalem", "Africa / Middle East", "IST-2IDT,M3.4.4/26,M10.5.0"},
    {"nairobi", "Africa / Middle East", "EAT-3"},
    {"istanbul", "Africa / Middle East", "<+03>-3"},
    {"riyadh", "Africa / Middle East", "<+03>-3"},
    {"tehran", "Africa / Middle East", "<+0330>-3:30"},
    {"dubai", "Africa / Middle East", "<+04>-4"},

    // Asia
    {"karachi", "Asia", "PKT-5"},
    {"kolkata", "Asia", "IST-5:30"},
    {"kathmandu", "Asia", "<+0545>-5:45"},
    {"dhaka", "Asia", "<+06>-6"},
    {"bangkok", "Asia", "<+07>-7"},
    {"jakarta", "Asia", "WIB-7"},
    {"singapore", "Asia", "<+08>-8"},
    {"hong_kong", "Asia", "HKT-8"},
    {"shanghai", "Asia", "CST-8"},
    {"taipei", "Asia", "CST-8"},
    {"manila", "Asia", "PST-8"},
    {"seoul", "Asia", "KST-9"},
    {"tokyo", "Asia", "JST-9"},

    // Oceania
    {"perth", "Oceania", "AWST-8"},
    {"adelaide", "Oceania", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"brisbane", "Oceania", "AEST-10"},
    {"sydney", "Oceania", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"melbourne", "Oceania", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"auckland", "Oceania", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"fiji", "Oceania", "<+12>-12"},

    // Universal
    {"utc", "Universal", "UTC0"},
};

// The names this project shipped with before the list went worldwide.  Kept
// working because they are documented and people have them in scripts, but
// left out of tzChoices so the picker offers one name per place.
static const struct {
  const char *alias;
  const char *of;
} tzAliases[] = {
    {"pacific", "los_angeles"}, {"mountain", "denver"},
    {"arizona", "phoenix"},     {"central", "chicago"},
    {"eastern", "new_york"},    {"alaska", "anchorage"},
    {"hawaii", "honolulu"},     {"uk", "london"},
    {"europe", "paris"},
};
const uint8_t numTzChoices = sizeof(tzChoices) / sizeof(tzChoices[0]);

const char *tzLookup(const char *name) {
  for (uint8_t i = 0; i < numTzChoices; i++)
    if (!strcasecmp(name, tzChoices[i].name))
      return tzChoices[i].posix;
  for (uint8_t i = 0; i < sizeof(tzAliases) / sizeof(tzAliases[0]); i++)
    if (!strcasecmp(name, tzAliases[i].alias))
      return tzLookup(tzAliases[i].of);
  return NULL;
}

void timeApplyTz(void) {
  // The C library reads TZ out of the environment, and caches it until
  // tzset() is called again -- so this has to run after every change, not
  // just once at boot.
  setenv("TZ", tzString, 1);
  tzset();
}

bool timeSetTz(const char *nameOrPosix) {
  if (!nameOrPosix || !*nameOrPosix)
    return false;
  // A shortcut name wins; anything else is taken as a POSIX string, which is
  // what lets somewhere not on the list still be set.
  const char *named = tzLookup(nameOrPosix);
  const char *want = named ? named : nameOrPosix;
  if (strlen(want) >= TZ_MAX)
    return false;
  strncpy(tzString, want, TZ_MAX - 1);
  tzString[TZ_MAX - 1] = '\0';
  timeApplyTz();
  return true;
}

void timeAccept(time_t utc, TimeSource from) {
  // Sources are ranked, and a lower-ranked one never overwrites a better one
  // that has already answered.  Without this, an RTC read that happens to
  // land after the first NTP reply would drag the clock back onto the less
  // accurate source.
  if (from < rank)
    return;
  struct timeval tv;
  tv.tv_sec = utc;
  tv.tv_usec = 0;
  settimeofday(&tv, NULL);
  source = from;
  rank = from;
  timeSynced = true;
}

void timeRelinquish(TimeSource from) {
  if (rank == from)
    rank = TIME_FREE; // the reading stays; nothing is defending it now
}

TimeSource timeSource(void) { return source; }

const char *timeSourceName(void) {
  switch (source) {
  case TIME_NTP:
    return "ntp";
  case TIME_MANUAL:
    return "manual";
  case TIME_RTC:
    return "rtc";
  default:
    return "free";
  }
}

bool timeLocal(struct tm &out) {
  if (!timeSynced)
    return false;
  time_t now = time(NULL);
  // Anything before 2021 means the system clock is still showing the 1970
  // it powers up with, whatever a source may have claimed.
  if (now < 1609459200L)
    return false;
  localtime_r(&now, &out);
  return true;
}

bool timeLocalSecOfDay(uint32_t &out) {
  struct tm t;
  if (!timeLocal(t))
    return false;
  out = (uint32_t)t.tm_hour * 3600UL + (uint32_t)t.tm_min * 60UL +
        (uint32_t)t.tm_sec;
  return true;
}

void timeReport(Print &p) {
  struct tm t;
  if (timeLocal(t))
    p.printf("  time %04d-%02d-%02d %02d:%02d:%02d  tz %s  from %s" "\n",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
             t.tm_sec, tzString, timeSourceName());
  else
    p.printf("  time not set (tz %s)" "\n", tzString);
}
