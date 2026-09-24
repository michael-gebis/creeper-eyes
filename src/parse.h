// Strict parsers for the text the console and the API accept.
//
// Strict meaning the whole string, or nothing.  atol("12abc") is 12 and
// atol("abc") is 0, and sscanf("%u") takes "-1" as four billion; each of
// those turned a typo into a valid-looking value somewhere.  These accept
// exactly the documented form and report failure for everything else, so a
// caller never has to wonder what a half-parsed string became.
//
// Ranges are checked here, on the parsed value, before anything narrows it:
// checking after a cast to uint8_t is how 300 became 44.

#ifndef PARSE_H
#define PARSE_H

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// A decimal integer in [lo, hi], optionally signed, nothing else around it.
static inline bool parseLong(const char *s, long lo, long hi, long &out) {
  if (!s || !*s || isspace((unsigned char)*s))
    return false; // strtol would skip leading space; we do not
  char *end = NULL;
  errno = 0;
  long v = strtol(s, &end, 10);
  if (errno || end == s || *end || v < lo || v > hi)
    return false;
  out = v;
  return true;
}

// Exactly six hex digits, with or without a leading '#'.
static inline bool parseHexColor(const char *s, uint32_t &out) {
  if (!s)
    return false;
  if (*s == '#')
    s++;
  uint32_t v = 0;
  for (uint8_t i = 0; i < 6; i++) {
    char c = s[i];
    if (!isxdigit((unsigned char)c))
      return false; // also catches the string ending early
    v = (v << 4) | (uint32_t)(isdigit((unsigned char)c) ? c - '0'
                                                        : (tolower(c) - 'a' + 10));
  }
  if (s[6])
    return false;
  out = v;
  return true;
}

// One or two decimal digits, for a field of a time.
static inline bool parseTimeField(const char *&s, unsigned max, unsigned &out) {
  if (!isdigit((unsigned char)s[0]))
    return false;
  unsigned v = (unsigned)(s[0] - '0');
  s++;
  if (isdigit((unsigned char)s[0])) {
    v = v * 10 + (unsigned)(s[0] - '0');
    s++;
  }
  if (v > max)
    return false;
  out = v;
  return true;
}

// "HH:MM", or "HH:MM:SS" when seconds are allowed.  24-hour.
static inline bool parseTimeOfDay(const char *s, bool allowSeconds,
                                  uint8_t &h, uint8_t &m, uint8_t &sec) {
  if (!s)
    return false;
  unsigned hh, mm, ss = 0;
  if (!parseTimeField(s, 23, hh) || *s++ != ':' || !parseTimeField(s, 59, mm))
    return false;
  if (*s == ':' && allowSeconds) {
    s++;
    if (!parseTimeField(s, 59, ss))
      return false;
  }
  if (*s)
    return false;
  h = (uint8_t)hh;
  m = (uint8_t)mm;
  sec = (uint8_t)ss;
  return true;
}

#endif // PARSE_H
