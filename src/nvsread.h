// Reading settings back out of NVS without trusting them.
//
// NVS checks a CRC on every entry, so a torn write or a flipped bit reads as
// a missing key rather than as garbage.  What it cannot catch is a value that
// is well-formed and wrong: written by an older firmware with other rules, by
// a restored backup from another board, by a bug, or by anybody with a cable
// and esptool.  So every read here checks, besides presence:
//
//   - the stored type, since the same key written as another type is not
//     the value it looks like;
//   - the range, for numbers;
//   - the length and the bytes, for strings -- read into a buffer of the
//     caller's size and never anywhere else.  (The core's own
//     getString(key, String) copies the whole value into a stack array sized
//     by whatever the entry claims, up to ~4000 bytes on the 8 KB loop stack.
//     Nothing here calls it.)
//
// A value that fails is reported through health.h, removed so the next boot
// does not trip on it again, and replaced by the caller's default.  The
// removal needs the namespace opened read-write; opened read-only, the value
// is still ignored, and the note says it could not be removed.
//
// Messages name the key, never the value: some of these are passwords.

#ifndef NVSREAD_H
#define NVSREAD_H

#include <Preferences.h>
#include <stddef.h>
#include <stdint.h>

// Stored by putBool(), which writes a u8 of 0 or 1.  Anything else in it is
// damage, not a third truth value.
bool nvsReadBool(Preferences &p, const char *key, bool def);

// Each in [lo, hi], stored as exactly that width.
uint8_t nvsReadU8(Preferences &p, const char *key, uint8_t def, uint8_t lo,
                  uint8_t hi);
uint16_t nvsReadU16(Preferences &p, const char *key, uint16_t def,
                    uint16_t lo, uint16_t hi);
uint32_t nvsReadU32(Preferences &p, const char *key, uint32_t def,
                    uint32_t lo, uint32_t hi);

// A string with no control characters that fits `n` bytes with its
// terminator.  True with the value in `out`; false with `out` empty if the
// key is absent or its value was unusable (and has been dealt with).
// `n` must be at least 1.
bool nvsReadStr(Preferences &p, const char *key, char *out, size_t n);

// For a value that read cleanly but fails a check only the caller can make
// -- a timezone that does not parse, a hash that is not hex.  Reports it,
// with `why`, and removes the key.
void nvsReject(Preferences &p, const char *key, const char *why);

#endif // NVSREAD_H
