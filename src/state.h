// What the device can be asked to do.
//
// This is the seam between the two front ends and the device itself.  The
// serial console parses words and calls these; the REST API parses JSON and
// calls the same ones.  Neither knows the other exists, and neither can drift
// from the other, because there is only one implementation of each operation.
//
// Before this existed the console *was* the interface, and the web server
// worked by handing it synthesised command strings -- which meant every HTTP
// call round-tripped through a text parser, and the console's human-readable
// output was load-bearing for machines.
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

#include <stdint.h>

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
  bool startleActive;

  bool clockOn;
  bool clockSeconds;
  uint16_t clockRate;      // free-running multiplier; ignored once NTP syncs
  uint32_t clockSecOfDay;  // current time as seconds past midnight
  uint32_t clockColor[3];  // 0xRRGGBB, hour / minute / second

  bool settingsDirty; // live settings differ from what is stored

  uint16_t fps;
  uint32_t freeHeap;
  uint32_t uptimeSec;
};

void stateGet(DeviceState &out);

// ------------------------------------------------------------- eye designs --

uint8_t stateEyeCount(void);
const char *stateEyeName(uint8_t index); // NULL if out of range
bool stateSetEyeIndex(uint8_t index);
bool stateSetEyeName(const char *name);
void stateNextEye(void);

// -------------------------------------------------------------------- gaze --

// x and y are 0-1023; anything outside that is rejected.
bool stateSetGaze(int16_t x, int16_t y);
void stateGazeAuto(void);

// ---------------------------------------------------------------- dilation --

// 0 is the narrowest pupil, 100 the widest.
bool stateSetDilation(uint8_t percent);
void stateDilationAuto(void);

// ------------------------------------------------------------------ pupil ---

void stateSetPupil(bool on);

// ------------------------------------------------------------------- panels --

// Exchanges the two panels' chip selects, for a pair wired the wrong way
// round.  Takes effect between frames.
void stateSetSwap(bool swapped);

// ---------------------------------------------------------------- one-shots --

void stateBlink(void);
void stateStartle(void);
void stateSplash(void);

// ------------------------------------------------------------------- clock --

void stateClockSetOn(bool on);
void stateClockSetSeconds(bool on);
bool stateClockSetRate(uint16_t rate);          // 1-3600
bool stateClockSetTime(uint8_t h, uint8_t m, uint8_t s);
// which: 0 hour, 1 minute, 2 second, -1 all three.  rgb is 0xRRGGBB.
bool stateClockSetColor(int8_t which, uint32_t rgb);

// ---------------------------------------------------------------- settings --

// Mark the live settings as differing from the stored ones, for a change
// made somewhere that does not own the flag.
void stateMarkDirty(void);

void stateSave(void);
void stateForget(void);

#endif // STATE_H
