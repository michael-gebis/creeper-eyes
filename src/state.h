// What the device can be asked to do.
//
// This is the seam between the two front ends and the device itself.  The
// serial console parses words and calls these; the REST API parses JSON and
// calls the same ones.  Neither knows the other exists, and neither can drift
// from the other, because there is only one implementation of each operation.
//
// The alternative, the web server handing the console synthesised command
// strings, would make every HTTP call round-trip through a text parser and
// the console's human-readable output load-bearing for machines.
//
// Operations that can fail return false rather than printing.  Deciding what
// to say about a failure belongs to the front end: the console prints prose,
// the API returns a status code.
//
// Implemented in main.cpp, beside the state it manipulates and the renderer
// that reads it.  Splitting the two apart would mean publishing twenty
// globals to move ten functions, which is more coupling, not less.

#ifndef STATE_H
#define STATE_H

#include <stddef.h>
#include <stdint.h>

#include "eyestore.h" // EyeLoadResult

// Numbers arrive as `long`, whatever they end up stored as, and each
// operation checks its own range on that.  Taking the narrow type instead
// made every caller cast first -- and a cast before the check is how a
// dilation of 300 became 44 and an eye index of 256 became 0.

// ---------------------------------------------------------------- snapshot --

// A consistent read of everything worth reporting.  Taken in one call so a
// status page cannot show a half-updated mixture.
struct DeviceState {
  uint8_t eyeIndex;
  uint8_t eyeCount;
  const char *eyeName;

  bool gazeManual;
  int16_t gazeX, gazeY; // 0-1023 each, 512 is centre

  bool dilateManual;
  uint8_t dilatePercent; // 100 = fully dilated
  bool pupilOn;

  bool swapped;
  bool flipped[2]; // per eye as displayed: 0 is on your left, 1 your right

  uint8_t dimPercent;  // the brightness setting, 0 (off) to 100
  uint8_t dimShown;    // what the panels show now: fades, sleep, a sweep
  uint8_t dimGammaX10; // the curve, in tenths
  int8_t dimTrim[2];   // per eye as displayed, like flipped[]
  bool dimSweeping;
  bool startleActive;

  bool clockOn;
  bool clockSeconds;
  bool clockSuppressed; // on, but nothing knows the time, so nothing is drawn
  uint16_t clockRate;      // free-running multiplier; ignored while NTP or
                           // the RTC supplies the time
  uint32_t clockSecOfDay;  // current time as seconds past midnight
  uint32_t clockColor[3];  // 0xRRGGBB, hour / minute / second

  uint16_t cpuMhz;     // what the CPU runs at
  uint16_t cpuSetting; // what it will run at from the next boot: 160 or 240
  bool settingsDirty; // live settings differ from what is stored

  uint16_t fps;
  uint32_t freeHeap;
  uint32_t uptimeSec;
};

void stateGet(DeviceState &out);

// ------------------------------------------------------------- eye designs --

uint8_t stateEyeCount(void);
const char *stateEyeName(uint8_t index); // NULL if out of range
bool stateSetEyeIndex(long index);
bool stateSetEyeName(const char *name);
void stateNextEye(void);

// ------------------------------------------------------------- the eye slot --
// One design beyond the built-in ones, loaded from a file into flash of its
// own.  See eyestore.h and docs/EYE_FILES.md.

struct EyeSlotState {
  bool available;    // the board's partition table has a slot at all
  bool loaded;       // and it holds a design
  const char *name;  // NULL while empty
  uint8_t index;     // where the design sits in the list while loaded
  uint32_t capacity; // bytes
};

void stateEyeSlot(EyeSlotState &out);

// An upload, streamed: Begin with the request's declared length, Chunk the
// body in as it arrives, End for the verdict -- or Abort if the connection
// goes.  On success the new design is selected.  From the web server only.
void stateEyeLoadBegin(uint32_t contentLength);
void stateEyeLoadChunk(const uint8_t *data, size_t n);
EyeLoadResult stateEyeLoadEnd(void);
void stateEyeLoadAbort(void);

// Empties the slot, moving the eyes to design 0 first if they were showing
// it.  False if the board has no slot, or the erase failed.
bool stateEyeUnload(void);

// -------------------------------------------------------------------- gaze --

// x and y are 0-1023; anything outside that is rejected.
bool stateSetGaze(long x, long y);
void stateGazeAuto(void);

// ---------------------------------------------------------------- dilation --

// 0 is the narrowest pupil, 100 the widest.
bool stateSetDilation(long percent);
void stateDilationAuto(void);

// ------------------------------------------------------------------ pupil ---

void stateSetPupil(bool on);

// ------------------------------------------------------------------- panels --

// Exchanges the two panels' chip selects, for a pair wired the wrong way
// round.  Takes effect between frames.
void stateSetSwap(bool swapped);

// Turns one panel's image through 180 degrees, for a panel mounted upside
// down.  `eye` is the eye as displayed -- 0 on your left, 1 on your right --
// but the setting attaches to the panel that eye is on and stays with it
// through a later swap.  False if there is no such eye.  Between frames.
bool stateSetFlip(uint8_t eye, bool flipped);

// ---------------------------------------------------------------- CPU speed --

// 160 or 240 MHz, stored at once and applied at the next boot: switching
// live re-locks the PLL, which took the network down for seconds.  False if
// the value is neither, or it could not be stored.  A board that restarts
// after a brownout comes back at 160 and stores that -- see loadSettings().
bool stateSetCpu(long mhz);

// Restarts the board a moment from now, after the caller has answered.
// Unsaved settings are lost; the reply is the place to say so.
void stateRestart(void);

// -------------------------------------------------------------- brightness --
// See dimmer.h.  0 is off; changes fade.  Each false if out of range.

bool stateDimSet(long percent);          // 0-100
bool stateDimSetGamma(long gammaX10);    // DIM_GAMMA_MIN-DIM_GAMMA_MAX, tenths
// `eye` as displayed, like stateSetFlip(), and like it the trim attaches to
// the panel that eye is on.  -50 to +50, a percentage of that panel's level.
bool stateDimSetTrim(uint8_t eye, long percent);
// The slow full-range sweep, for judging a curve by eye.  Not a setting:
// it is not saved, and setting a level ends it.
void stateDimSweep(bool on);

// ---------------------------------------------------------------- one-shots --

void stateBlink(void);
void stateStartle(void);
void stateSplash(void);

// ------------------------------------------------------------------- clock --

void stateClockSetOn(bool on);
void stateClockSetSeconds(bool on);
bool stateClockSetRate(long rate);              // 1-3600
bool stateClockSetTime(long h, long m, long s);
// which: 0 hour, 1 minute, 2 second, -1 all three.  rgb is 0xRRGGBB.
bool stateClockSetColor(long which, uint32_t rgb);

// ------------------------------------------------------- when they happen --
//
// An operation can arrive in the middle of a frame.  The console is polled
// from frame(), and so is the web server, so both can land between any two
// things the renderer does -- including halfway through drawing an eye.
//
// So: anything the renderer reads *while* drawing is not written directly.
// It is queued, and applied between frames by statePollPending().  The eye
// design is the case that matters -- drawEye() dereferences five artwork
// pointers per pixel, and swapping them underneath it tears a frame -- and
// the panel swap already worked this way.
//
// The SPI bus follows the same rule for the same reason.  Operations that
// paint, like the address cards, queue a request and let the renderer draw
// it, rather than pushing a canvas from under its feet.
//
// The lock is belt to that brace.  It costs nothing at this scale, it makes
// the operations safe to call from somewhere other than the render loop
// should that ever happen, and it is the memory barrier that makes the
// queueing above mean what it says.
//
// It serializes writers; the renderer does not take it.  frame() reads the
// gaze, dilation, pupil and startle settings directly.  Each is a word or
// less, written whole, so a read sees the old value or the new one -- at
// worst the two halves of a gaze position disagree for one frame.

// Apply anything queued.  Called from the render loop between frames, and
// only from there.
void statePollPending(void);

// Held for the duration of an operation.
void stateLock(void);
void stateUnlock(void);

// ---------------------------------------------------------------- settings --

// Mark the live settings as differing from the stored ones, for a change
// made somewhere that does not own the flag.
void stateMarkDirty(void);

void stateSave(void);
void stateForget(void);

#endif // STATE_H
