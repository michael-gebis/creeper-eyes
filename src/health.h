// What the board found wrong at boot, kept for whoever looks: problems in its
// own stored data, and a restart it did not plan.
//
// Everything loaded from flash is checked as it is read -- see nvsread.h for
// the settings and credentials, eyestore.h for the eye slot, net.cpp for the
// radio's stored network.  A value that fails is ignored, a default takes its
// place, and where the fix is obvious the stored copy is repaired.  This is
// where each of those says what it found and what it did, so the control
// page and the console can show it rather than the board quietly behaving
// differently from how it was set up.
//
// An unplanned restart -- a crash, a watchdog, a brownout -- is noted here
// too, by reportResetReason() in main.cpp.  Its reason is only known at the
// next boot, and printed to serial alone it would scroll past before anyone
// was listening; here it waits on the control page instead.
//
// Boot-time by nature, so the list only grows; it is cleared by a restart.

#ifndef HEALTH_H
#define HEALTH_H

#include <stdint.h>

// Room for this many; later ones are counted but not kept.
#define HEALTH_MAX 8
#define HEALTH_LINE 96 // bytes per line, terminator included

// One line, printf-style, e.g. "stored clock rate 0 is out of range; reset
// to 1".  Also printed to the debug console.
void healthNote(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

uint8_t healthCount(void);      // lines kept, at most HEALTH_MAX
uint8_t healthDropped(void);    // lines noted past that
const char *healthLine(uint8_t i); // NULL past the end

#endif // HEALTH_H
