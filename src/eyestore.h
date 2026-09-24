// The eye slot: one design beyond those compiled in, loaded from a file into
// a flash partition of its own.  docs/EYE_FILES.md has the file format and
// the reasoning behind it.
//
// This module knows about flash and the file format and nothing else.  It
// does not know which designs are built in or which one is on screen; the
// registry in main.cpp does, and it is consulted through the gate passed to
// eyeStoreWriteBegin() at the one moment that matters -- after a file has
// been judged good and before the slot is erased to make room for it.
//
// Everything here runs in the render task.  The web server and the console
// are both served from inside frame(), so there is no second caller to
// guard against.

#ifndef EYESTORE_H
#define EYESTORE_H

#include <stddef.h>
#include <stdint.h>

// Format version 1.  The table sizes are the renderer's, and main.cpp
// checks at compile time that the two agree.
#define EYE_FILE_VERSION 1
#define EYE_FILE_HEADER_BYTES 64
#define EYE_FILE_NAME_MAX 15 // characters, not counting the terminator

#define EYE_FILE_SCLERA_BYTES (200 * 200 * 2)
#define EYE_FILE_IRIS_BYTES (64 * 256 * 2)
#define EYE_FILE_UPPER_BYTES (128 * 128)
#define EYE_FILE_LOWER_BYTES (128 * 128)
#define EYE_FILE_POLAR_BYTES (80 * 80 * 2)
#define EYE_FILE_PAYLOAD_BYTES                                                \
  (EYE_FILE_SCLERA_BYTES + EYE_FILE_IRIS_BYTES + EYE_FILE_UPPER_BYTES +       \
   EYE_FILE_LOWER_BYTES + EYE_FILE_POLAR_BYTES)
#define EYE_FILE_BYTES (EYE_FILE_HEADER_BYTES + EYE_FILE_PAYLOAD_BYTES)

// The five tables, pointing into mapped flash.  Untyped here; main.cpp gives
// them the array-of-rows types the renderer reads through.
struct EyeArt {
  const uint16_t *sclera;
  const uint16_t *iris;
  const uint8_t *upper;
  const uint8_t *lower;
  const uint16_t *polar;
};

enum EyeLoadResult {
  EYE_LOAD_OK,
  EYE_LOAD_NO_SLOT,      // this board's partition table has no `eyes`
  EYE_LOAD_NOT_EYE_FILE, // wrong magic, or too short to have a header
  EYE_LOAD_VERSION,      // an eye file, in a format this firmware predates
  EYE_LOAD_SIZE,         // a length in the header or on the wire is wrong
  EYE_LOAD_NAME,         // empty, too long, or not a-z0-9 from a letter
  EYE_LOAD_NAME_TAKEN,   // a built-in design or a console keyword has it
  EYE_LOAD_HASH,         // the payload is not the one the header describes
  EYE_LOAD_INCOMPLETE,   // the upload stopped early
  EYE_LOAD_FLASH,        // erasing, writing or mapping failed
};

// One line of prose per result, for the console and the API alike.
const char *eyeLoadResultText(EyeLoadResult r);

// Finds, maps and verifies the slot.  Once, from setup().
void eyeStoreBegin(void);

// Whether this board has the partition at all.  A board updated over the
// air from an older firmware keeps its older partition table, and has not.
bool eyeStoreAvailable(void);
uint32_t eyeStoreCapacity(void); // bytes; 0 without a slot

// The design in the slot.  Both false/NULL while it is empty.  The name and
// the tables stay valid until the next write or erase -- which is why the
// gate below exists.
const char *eyeStoreName(void);
bool eyeStoreArt(EyeArt &out);

// Asked once per upload, with the name from a header that has passed every
// other check, just before the slot is erased.  Returns EYE_LOAD_OK to let
// the write go ahead, having first stopped anything reading the old design;
// anything else refuses the file and leaves the slot as it was.
typedef EyeLoadResult (*EyeWriteGate)(const char *name);

// A streamed write.  Begin with the request's Content-Length, feed the body
// in whatever pieces it arrives in, then End for the verdict.  Nothing is
// erased until the header has been read and approved, and the header is
// written only once the payload hashes correctly, so every way of stopping
// early leaves either the old design or an empty slot -- never half of one.
void eyeStoreWriteBegin(uint32_t contentLength, EyeWriteGate gate);
void eyeStoreWrite(const uint8_t *data, size_t n);
EyeLoadResult eyeStoreWriteEnd(void);
void eyeStoreWriteAbort(void); // the connection went away

// Empties the slot.  The caller must already have stopped the renderer
// using it.  False only if there is no slot or the erase failed.
bool eyeStoreErase(void);

#endif // EYESTORE_H
