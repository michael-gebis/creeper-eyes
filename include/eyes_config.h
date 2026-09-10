// Which eye designs are compiled into the firmware.
//
// Each design costs roughly 158 KB of flash, so the default 1.25 MB app
// partition fits about four alongside everything else.  Enable only what you
// want on the head; the console switches between whatever is built in.
//
// Two ways to configure:
//
//   1. Edit the switches below.
//   2. Override from platformio.ini without touching this file, e.g.
//
//        build_flags = -DEYE_DEFAULT=0 -DEYE_DRAGON=1 -DEYE_SKULL=1
//
//      Every switch is #ifndef-guarded, so a -D always wins.
//
// See docs/EYES.md for a rendered gallery of every design.
//
// ADDING A NEW DESIGN
//
//   1. Generate its header with tools/gen_eyes.py, or hand-write one whose
//      symbols are suffixed to match -- scleraFoo, irisFoo, upperFoo,
//      lowerFoo, polarFoo.
//   2. Add an EYE_FOO switch here.
//   3. Add the #include and the registry row in src/main.cpp, both guarded
//      by #if EYE_FOO.
//
//   Dimensions must match: SCLERA 200x200, IRIS_MAP 256x64, SCREEN 128x128,
//   IRIS 80x80.  The renderer reaches the artwork through pointers whose row
//   width is fixed at compile time, so designs of different sizes cannot
//   coexist in one build.

#ifndef EYES_CONFIG_H
#define EYES_CONFIG_H

// Built in by default -----------------------------------------------------

#ifndef EYE_DEFAULT
#define EYE_DEFAULT 1 // Standard human-ish hazel eye
#endif

#ifndef EYE_NEWT
#define EYE_NEWT 1 // Eye of newt
#endif

// From the TeensyEyes project (MIT) -- off by default ---------------------

#ifndef EYE_ANIME
#define EYE_ANIME 0 // Large violet anime iris
#endif

#ifndef EYE_BIGBLUE
#define EYE_BIGBLUE 0 // Pale blue, heavy limbal ring
#endif

#ifndef EYE_BLUEFLAME1
#define EYE_BLUEFLAME1 0 // Blue flame ring on black
#endif

#ifndef EYE_BLUEFLAME2
#define EYE_BLUEFLAME2 0 // Blue flame, slit pupil
#endif

#ifndef EYE_BROWN
#define EYE_BROWN 0 // Warm brown, veined sclera
#endif

#ifndef EYE_CAT
#define EYE_CAT 0 // Yellow cat eye, slit pupil
#endif

#ifndef EYE_DEMON
#define EYE_DEMON 0 // Red demon, slit pupil
#endif

#ifndef EYE_DOE
#define EYE_DOE 0 // Soft brown doe eye
#endif

#ifndef EYE_DOOMRED
#define EYE_DOOMRED 0 // Red on white, cartoon
#endif

#ifndef EYE_DOOMSPIRAL
#define EYE_DOOMSPIRAL 0 // Red spiral
#endif

#ifndef EYE_DRAGON
#define EYE_DRAGON 0 // Fiery dragon, slit pupil
#endif

#ifndef EYE_FIREBOX
#define EYE_FIREBOX 0 // Orange fire ring
#endif

#ifndef EYE_FISH
#define EYE_FISH 0 // Pale fish eye, no eyelids
#endif

#ifndef EYE_FIZZGIG
#define EYE_FIZZGIG 0 // Orange fizzgig
#endif

#ifndef EYE_FLAME
#define EYE_FLAME 0 // Flame iris, slit pupil
#endif

#ifndef EYE_HAZEL
#define EYE_HAZEL 0 // Hazel, veined sclera
#endif

#ifndef EYE_HYPNORED
#define EYE_HYPNORED 0 // Red hypnotic rings
#endif

#ifndef EYE_LEOPARD
#define EYE_LEOPARD 0 // Golden leopard
#endif

#ifndef EYE_NEWT2
#define EYE_NEWT2 0 // Eye of newt (TeensyEyes)
#endif

#ifndef EYE_SKULL
#define EYE_SKULL 0 // Red on bone, no eyelids
#endif

#ifndef EYE_SNAKEGREEN
#define EYE_SNAKEGREEN 0 // Green snake, slit pupil
#endif

#ifndef EYE_SPIKES
#define EYE_SPIKES 0 // Geometric spikes
#endif

#ifndef EYE_TOONSTRIPE
#define EYE_TOONSTRIPE 0 // Striped cartoon, no eyelids
#endif

// At least one design has to be present: the eye headers are also where the
// SCLERA_*, IRIS_* and SCREEN_* dimensions come from, so a build with none
// would not have them defined.
#if !(EYE_DEFAULT || EYE_NEWT || EYE_ANIME || EYE_BIGBLUE || EYE_BLUEFLAME1 || EYE_BLUEFLAME2 || EYE_BROWN || EYE_CAT || EYE_DEMON || EYE_DOE || EYE_DOOMRED || EYE_DOOMSPIRAL || EYE_DRAGON || EYE_FIREBOX || EYE_FISH || EYE_FIZZGIG || EYE_FLAME || EYE_HAZEL || EYE_HYPNORED || EYE_LEOPARD || EYE_NEWT2 || EYE_SKULL || EYE_SNAKEGREEN || EYE_SPIKES || EYE_TOONSTRIPE)
#error "Enable at least one eye design in include/eyes_config.h"
#endif

#endif // EYES_CONFIG_H
