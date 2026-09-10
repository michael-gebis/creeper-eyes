// Which eye designs are compiled into the firmware.
//
// Each design costs roughly 165 KB of flash, so the default 1.25 MB app
// partition fits about four alongside everything else.  Enable only what you
// want on the head; the console can switch between whatever is built in.
//
// Two ways to configure:
//
//   1. Edit the defaults below.
//   2. Override from platformio.ini without touching this file, e.g.
//
//        build_flags = -DEYE_DEFAULT=0 -DEYE_NEWT=1
//
//      Every switch is #ifndef-guarded, so a -D always wins.
//
// ADDING A NEW DESIGN
//
//   1. Drop its header in include/, with symbols suffixed to match the
//      existing ones -- scleraFoo, irisFoo, upperFoo, lowerFoo, polarFoo.
//   2. Add an EYE_FOO switch here.
//   3. Add the #include and the registry row in src/main.cpp, both guarded
//      by #if EYE_FOO.
//
//   The dimensions must match what is already compiled in: SCLERA 200x200,
//   IRIS_MAP 256x64, SCREEN 128x128, IRIS 80x80.  The renderer reaches the
//   artwork through pointers whose row width is fixed at compile time, so
//   designs of different sizes cannot coexist in one build.  Adafruit's
//   convert/tablegen.py can re-render any source art at these dimensions.

#ifndef EYES_CONFIG_H
#define EYES_CONFIG_H

#ifndef EYE_DEFAULT
#define EYE_DEFAULT 1 // Standard human-ish hazel eye
#endif

#ifndef EYE_NEWT
#define EYE_NEWT 1 // Eye of newt
#endif

// At least one design has to be present: the eye headers are also where the
// SCLERA_*, IRIS_* and SCREEN_* dimensions come from, so a build with none
// would not have them defined.
#if !(EYE_DEFAULT || EYE_NEWT)
#error "Enable at least one eye design in include/eyes_config.h"
#endif

#endif // EYES_CONFIG_H
