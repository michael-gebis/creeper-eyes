// See nvsread.h.

#include "nvsread.h"

#include "health.h"
#include <string.h>

void nvsReject(Preferences &p, const char *key, const char *why) {
  if (p.remove(key))
    healthNote("stored %s %s; reset to the default", key, why);
  else
    healthNote("stored %s %s; ignored, but could not be removed", key, why);
}

// Present, and stored as `want`?  Absent is the ordinary case and says
// nothing; the wrong type is reported and removed.
static bool present(Preferences &p, const char *key, PreferenceType want) {
  PreferenceType t = p.getType(key);
  if (t == PT_INVALID)
    return false; // no such key: the default applies, and that is normal
  if (t != want) {
    nvsReject(p, key, "was saved as the wrong type");
    return false;
  }
  return true;
}

// The three widths differ only in the getter, so they share this.
static uint32_t readRanged(Preferences &p, const char *key, PreferenceType t,
                           uint32_t def, uint32_t lo, uint32_t hi) {
  if (!present(p, key, t))
    return def;
  uint32_t v = t == PT_U8    ? p.getUChar(key, (uint8_t)def)
               : t == PT_U16 ? p.getUShort(key, (uint16_t)def)
                             : p.getULong(key, def);
  if (v < lo || v > hi) {
    char why[48];
    snprintf(why, sizeof(why), "is %lu, outside %lu-%lu", (unsigned long)v,
             (unsigned long)lo, (unsigned long)hi);
    nvsReject(p, key, why);
    return def;
  }
  return v;
}

bool nvsReadBool(Preferences &p, const char *key, bool def) {
  return readRanged(p, key, PT_U8, def ? 1 : 0, 0, 1) == 1;
}

uint8_t nvsReadU8(Preferences &p, const char *key, uint8_t def, uint8_t lo,
                  uint8_t hi) {
  return (uint8_t)readRanged(p, key, PT_U8, def, lo, hi);
}

uint16_t nvsReadU16(Preferences &p, const char *key, uint16_t def,
                    uint16_t lo, uint16_t hi) {
  return (uint16_t)readRanged(p, key, PT_U16, def, lo, hi);
}

uint32_t nvsReadU32(Preferences &p, const char *key, uint32_t def,
                    uint32_t lo, uint32_t hi) {
  return readRanged(p, key, PT_U32, def, lo, hi);
}

int8_t nvsReadI8(Preferences &p, const char *key, int8_t def, int8_t lo,
                 int8_t hi) {
  if (!present(p, key, PT_I8))
    return def;
  int8_t v = p.getChar(key, def);
  if (v < lo || v > hi) {
    char why[40];
    snprintf(why, sizeof(why), "is %d, outside %d-%d", v, lo, hi);
    nvsReject(p, key, why);
    return def;
  }
  return v;
}

bool nvsReadStr(Preferences &p, const char *key, char *out, size_t n) {
  out[0] = '\0';
  if (!present(p, key, PT_STR))
    return false;
  // The bounded getter: it asks NVS for the length first and refuses, with
  // 0, anything that would not fit -- so nothing larger than `out` is ever
  // copied anywhere.  Its length includes the terminator.
  size_t got = p.getString(key, out, n);
  if (got == 0 || got > n) {
    out[0] = '\0';
    nvsReject(p, key, "is too long");
    return false;
  }
  out[n - 1] = '\0'; // NVS terminates it, but this buffer is ours to trust
  for (const char *c = out; *c; c++) {
    unsigned char u = (unsigned char)*c;
    if (u < 0x20 || u == 0x7f) {
      out[0] = '\0';
      nvsReject(p, key, "contains control characters");
      return false;
    }
  }
  return true;
}
