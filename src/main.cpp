//--------------------------------------------------------------------------
// 2018 Modified by Laurent Moll for Uncanny Eyes costume
// https://www.hackster.io/projects/376a13/
// Based on Adafruit code  - Adafruit header below:
//
// Uncanny eyes for PJRC Teensy 3.1 with Adafruit 1.5" OLED (product #1431)
// or 1.44" TFT LCD (#2088).  This uses Teensy-3.1-specific features and
// WILL NOT work on normal Arduino or other boards!  Use 72 MHz (Optimized)
// board speed -- OLED does not work at 96 MHz.
//
// Adafruit invests time and resources providing this open source code,
// please support Adafruit and open-source hardware by purchasing products
// from Adafruit!
//
// Written by Phil Burgess / Paint Your Dragon for Adafruit Industries.
// MIT license.  SPI FIFO insight from Paul Stoffregen's ILI9341_t3 library.
// Inspired by David Boccabella's (Marcwolf) hybrid servo/OLED eye concept.
//--------------------------------------------------------------------------

#include "config.h"
#include "console.h"
#include "state.h"
#include "display.h"
#include "net.h"
#include "sleepmode.h"
#include "credentials.h"
#include "rtc.h"
#include "timekeeping.h"
#include "eyestore.h"
#include "dimmer.h"
#include "health.h"
#include "nvsread.h"
#include "parse.h"

#include <Adafruit_GFX.h>   // Core graphics lib for Adafruit displays
#include <Preferences.h>    // NVS-backed settings, part of the ESP32 core
#if NETWORK
#include <WiFi.h>
#endif
#include <SPI.h>
#include <esp_freertos_hooks.h> // the tick hook behind wdtClue

// Which designs are built in -- see include/eyes_config.h.  The headers are
// modified from Adafruit's originals so that several can be included at once,
// each set of symbols carrying its own suffix.
#include "eyes_config.h"

#if EYE_DEFAULT
#include "defaultEye.h" // Standard human-ish hazel eye
#endif
#if EYE_NEWT
#include "newtEye.h" // Eye of newt
#endif
#if EYE_ANIME
#include "eyes/anime.h" // Large violet anime iris
#endif
#if EYE_BIGBLUE
#include "eyes/bigBlue.h" // Pale blue, heavy limbal ring
#endif
#if EYE_BLUEFLAME1
#include "eyes/blueFlame1.h" // Blue flame ring on black
#endif
#if EYE_BLUEFLAME2
#include "eyes/blueFlame2.h" // Blue flame, slit pupil
#endif
#if EYE_BROWN
#include "eyes/brown.h" // Warm brown, veined sclera
#endif
#if EYE_CAT
#include "eyes/cat.h" // Yellow cat eye, slit pupil
#endif
#if EYE_DEMON
#include "eyes/demon.h" // Red demon, slit pupil
#endif
#if EYE_DOE
#include "eyes/doe.h" // Soft brown doe eye
#endif
#if EYE_DOOMRED
#include "eyes/doomRed.h" // Red on white, cartoon
#endif
#if EYE_DOOMSPIRAL
#include "eyes/doomSpiral.h" // Red spiral
#endif
#if EYE_DRAGON
#include "eyes/dragon.h" // Fiery dragon, slit pupil
#endif
#if EYE_FIREBOX
#include "eyes/firebox.h" // Orange fire ring
#endif
#if EYE_FISH
#include "eyes/fish.h" // Pale fish eye, no eyelids
#endif
#if EYE_FIZZGIG
#include "eyes/fizzgig.h" // Orange fizzgig
#endif
#if EYE_FLAME
#include "eyes/flame.h" // Flame iris, slit pupil
#endif
#if EYE_HAZEL
#include "eyes/hazel.h" // Hazel, veined sclera
#endif
#if EYE_HYPNORED
#include "eyes/hypnoRed.h" // Red hypnotic rings
#endif
#if EYE_LEOPARD
#include "eyes/leopard.h" // Golden leopard
#endif
#if EYE_NEWT2
#include "eyes/newt2.h" // Eye of newt (TeensyEyes)
#endif
#if EYE_SKULL
#include "eyes/skull.h" // Red on bone, no eyelids
#endif
#if EYE_SNAKEGREEN
#include "eyes/snakeGreen.h" // Green snake, slit pupil
#endif
#if EYE_SPIKES
#include "eyes/spikes.h" // Geometric spikes
#endif
#if EYE_TOONSTRIPE
#include "eyes/toonstripe.h" // Striped cartoon, no eyelids
#endif

// The renderer reads the artwork through these.  They are pointers TO const
// data, not const pointers, so a whole design can be swapped at runtime.
// Swap between frames, never mid-render: drawEye() reads all five as it
// scans, so changing them under it would tear one frame.
const uint16_t (*sclera)[SCLERA_WIDTH];
const uint8_t (*upper)[SCREEN_WIDTH];
const uint8_t (*lower)[SCREEN_WIDTH];
const uint16_t (*polar)[IRIS_WIDTH];
const uint16_t (*iris)[IRIS_MAP_WIDTH];

// Registry of the designs compiled in.  Order here is the order the console
// reports and indexes them by.
typedef struct {
  const char *name;
  const uint16_t (*sclera)[SCLERA_WIDTH];
  const uint8_t (*upper)[SCREEN_WIDTH];
  const uint8_t (*lower)[SCREEN_WIDTH];
  const uint16_t (*polar)[IRIS_WIDTH];
  const uint16_t (*iris)[IRIS_MAP_WIDTH];
} EyeDesign;

static const EyeDesign eyeDesigns[] = {
#if EYE_DEFAULT
    {"default", scleraDefault, upperDefault, lowerDefault, polarDefault, irisDefault},
#endif
#if EYE_NEWT
    {"newt", scleraNewt, upperNewt, lowerNewt, polarNewt, irisNewt},
#endif
#if EYE_ANIME
    {"anime", scleraAnime, upperAnime, lowerAnime, polarAnime, irisAnime},
#endif
#if EYE_BIGBLUE
    {"bigblue", scleraBigBlue, upperBigBlue, lowerBigBlue, polarBigBlue, irisBigBlue},
#endif
#if EYE_BLUEFLAME1
    {"blueflame1", scleraBlueFlame1, upperBlueFlame1, lowerBlueFlame1, polarBlueFlame1, irisBlueFlame1},
#endif
#if EYE_BLUEFLAME2
    {"blueflame2", scleraBlueFlame2, upperBlueFlame2, lowerBlueFlame2, polarBlueFlame2, irisBlueFlame2},
#endif
#if EYE_BROWN
    {"brown", scleraBrown, upperBrown, lowerBrown, polarBrown, irisBrown},
#endif
#if EYE_CAT
    {"cat", scleraCat, upperCat, lowerCat, polarCat, irisCat},
#endif
#if EYE_DEMON
    {"demon", scleraDemon, upperDemon, lowerDemon, polarDemon, irisDemon},
#endif
#if EYE_DOE
    {"doe", scleraDoe, upperDoe, lowerDoe, polarDoe, irisDoe},
#endif
#if EYE_DOOMRED
    {"doomred", scleraDoomRed, upperDoomRed, lowerDoomRed, polarDoomRed, irisDoomRed},
#endif
#if EYE_DOOMSPIRAL
    {"doomspiral", scleraDoomSpiral, upperDoomSpiral, lowerDoomSpiral, polarDoomSpiral, irisDoomSpiral},
#endif
#if EYE_DRAGON
    {"dragon", scleraDragon, upperDragon, lowerDragon, polarDragon, irisDragon},
#endif
#if EYE_FIREBOX
    {"firebox", scleraFirebox, upperFirebox, lowerFirebox, polarFirebox, irisFirebox},
#endif
#if EYE_FISH
    {"fish", scleraFish, upperFish, lowerFish, polarFish, irisFish},
#endif
#if EYE_FIZZGIG
    {"fizzgig", scleraFizzgig, upperFizzgig, lowerFizzgig, polarFizzgig, irisFizzgig},
#endif
#if EYE_FLAME
    {"flame", scleraFlame, upperFlame, lowerFlame, polarFlame, irisFlame},
#endif
#if EYE_HAZEL
    {"hazel", scleraHazel, upperHazel, lowerHazel, polarHazel, irisHazel},
#endif
#if EYE_HYPNORED
    {"hypnored", scleraHypnoRed, upperHypnoRed, lowerHypnoRed, polarHypnoRed, irisHypnoRed},
#endif
#if EYE_LEOPARD
    {"leopard", scleraLeopard, upperLeopard, lowerLeopard, polarLeopard, irisLeopard},
#endif
#if EYE_NEWT2
    {"newt2", scleraNewt2, upperNewt2, lowerNewt2, polarNewt2, irisNewt2},
#endif
#if EYE_SKULL
    {"skull", scleraSkull, upperSkull, lowerSkull, polarSkull, irisSkull},
#endif
#if EYE_SNAKEGREEN
    {"snakegreen", scleraSnakeGreen, upperSnakeGreen, lowerSnakeGreen, polarSnakeGreen, irisSnakeGreen},
#endif
#if EYE_SPIKES
    {"spikes", scleraSpikes, upperSpikes, lowerSpikes, polarSpikes, irisSpikes},
#endif
#if EYE_TOONSTRIPE
    {"toonstripe", scleraToonstripe, upperToonstripe, lowerToonstripe, polarToonstripe, irisToonstripe},
#endif
};

#define NUM_BUILTIN_DESIGNS (sizeof(eyeDesigns) / sizeof(eyeDesigns[0]))

// The design in the eye slot, loaded from a file rather than compiled in --
// see eyestore.h.  Always listed after the built-ins, at this index, so
// loading or removing it never renumbers them.
#define LOADED_DESIGN NUM_BUILTIN_DESIGNS
static EyeDesign loadedDesign;
static bool loadedPresent = false;

// The file format fixes the table sizes; the renderer takes them from the
// headers.  A disagreement would have every loaded design read with the
// wrong row width, so it is made a build failure instead.
static_assert(SCLERA_WIDTH * SCLERA_HEIGHT * 2 == EYE_FILE_SCLERA_BYTES,
              "eye file sclera size");
static_assert(IRIS_MAP_WIDTH * IRIS_MAP_HEIGHT * 2 == EYE_FILE_IRIS_BYTES,
              "eye file iris size");
static_assert(SCREEN_WIDTH * SCREEN_HEIGHT == EYE_FILE_UPPER_BYTES &&
                  SCREEN_WIDTH * SCREEN_HEIGHT == EYE_FILE_LOWER_BYTES,
              "eye file eyelid size");
static_assert(IRIS_WIDTH * IRIS_HEIGHT * 2 == EYE_FILE_POLAR_BYTES,
              "eye file polar size");

static uint8_t designCount(void) {
  return NUM_BUILTIN_DESIGNS + (loadedPresent ? 1 : 0);
}

// Callers bound the index by designCount() first.
static const EyeDesign &designAt(uint8_t i) {
  return i < NUM_BUILTIN_DESIGNS ? eyeDesigns[i] : loadedDesign;
}

// Lists whatever the slot holds.  At boot, and after an upload succeeds.
static void adoptLoadedDesign(void) {
  EyeArt a;
  loadedPresent = eyeStoreArt(a);
  if (!loadedPresent)
    return;
  loadedDesign.name = eyeStoreName();
  loadedDesign.sclera = (const uint16_t(*)[SCLERA_WIDTH])a.sclera;
  loadedDesign.upper = (const uint8_t(*)[SCREEN_WIDTH])a.upper;
  loadedDesign.lower = (const uint8_t(*)[SCREEN_WIDTH])a.lower;
  loadedDesign.polar = (const uint16_t(*)[IRIS_WIDTH])a.polar;
  loadedDesign.iris = (const uint16_t(*)[IRIS_MAP_WIDTH])a.iris;
}

static uint8_t eyeDesign = 0;

// Whether the eye has a pupil at all.  Off gives a full iris disc, which
// suits a clock face but is not tied to it.
static bool pupilOn = true;

// Panel assignment.  Swapping the chip-select pins moves everything that
// belongs to an eye -- its mirrored eyelids and its splash label -- to the
// other physical panel, which is what makes this a real fix for a miswire
// rather than a cosmetic one.
static bool eyesSwapped = false;
static bool swapPending = false;

// The CPU speed setting in MHz, 160 or 240: what the CPU is set to at the
// next boot.  The build's board_build.f_cpu is the default; a stored setting
// takes over when the settings load, which is before WiFi starts.  It is not
// switched live, because 160 and 240 come from different PLL frequencies:
// re-locking the PLL with the radio up cost 8.5 s of network every time it
// was tried.  240 draws faster, and draws more current -- docs/FRAME_RATE.md.
static uint8_t cpuSetting = F_CPU / 1000000;

static bool cpuValid(long mhz) { return mhz == 160 || mhz == 240; }

// A restart asked for over the console or the API.  Answered first and done
// from the render loop a moment later, so the caller gets its reply rather
// than a dropped connection it cannot tell from a crash.  A software restart,
// so the warnings list says nothing about it afterwards.
#define RESTART_DELAY_MS 1000
static bool restartPending = false;
static uint32_t restartAskedMs = 0;

// Which panels were mounted upside down.  Indexed by chip-select slot --
// 0 for SELECT_L_PIN, 1 for SELECT_R_PIN -- and NOT by eye, because it is
// the panel that is the wrong way up, and it stays that way whichever eye
// is drawn on it.  A later `swap` moves the eyes and leaves these where
// they are, which is the only arrangement in which the two settings do
// not fight.  The rotation itself is done in the panel controller, see
// applyFlips(); nothing in the frame path knows about it.
static bool panelFlip[2] = {false, false};
static bool flipPending = false;

#if CLOCK

static bool clockOn = false;
static bool clockSeconds = true;
static uint16_t clockRate = 1;              // 1 = real time
static uint32_t clockBaseSec = 10 * 3600UL + 10 * 60UL; // 10:10, watch-ad time
static uint32_t clockBaseMs = 0;
static uint16_t clockHourAng, clockMinAng, clockSecAng;

// [0] hour, [1] minute, [2] second.  The 24-bit copies are kept only so
// `clock` can report what was asked for rather than the lossy 565 value.
static uint32_t clockRGB[3] = {CLOCK_HOUR_COLOR, CLOCK_MIN_COLOR,
                              CLOCK_SEC_COLOR};
static uint16_t clockPix[3];

// RRGGBB to RGB565: keep the top 5, 6 and 5 bits and pack them.
static inline uint16_t rgb24to565(uint32_t v) {
  return (uint16_t)(((v >> 8) & 0xF800) | ((v >> 5) & 0x07E0) |
                    ((v >> 3) & 0x001F));
}

// Keeps both representations: the 565 one is what gets drawn, the 24-bit
// one only so `clock` can echo back what was actually asked for.
static void clockSetColor(uint8_t which, uint32_t rgb) {
  clockRGB[which] = rgb & 0xFFFFFF;
  clockPix[which] = rgb24to565(clockRGB[which]);
}

// Rebases the free-running origin.  Everything else derives the current
// time from this pair, so setting the clock is just moving the origin.
static void clockSet(uint32_t secOfDay) {
  clockBaseSec = secOfDay % 86400UL;
  clockBaseMs = millis();
}

// Unit direction of each hand, 8.8 fixed point.  Recomputed once per frame
// rather than per pixel -- six trig calls a frame is nothing.
static int16_t clockDirX[3], clockDirY[3];

// The polar table's angle convention: a = (atan2(dy, dx) + PI) / 2PI * 512,
// which puts 12 o'clock at 128 and runs clockwise.
static void clockDirs(void) {
  const uint16_t ang[3] = {clockHourAng, clockMinAng, clockSecAng};
  for (uint8_t i = 0; i < 3; i++) {
    float th = (float)ang[i] * (2.0f * 3.14159265f / 512.0f) - 3.14159265f;
    clockDirX[i] = (int16_t)(cosf(th) * 256.0f);
    clockDirY[i] = (int16_t)(sinf(th) * 256.0f);
  }
}

// Recomputes the three hand angles and their direction vectors.  Called
// once per frame while the clock is on, never per pixel.
static uint32_t clockNow(void); // defined just below

static void clockUpdate(void) {
  uint32_t t = clockNow();
  uint32_t sec = t % 60, min = (t / 60) % 60, hr = (t / 3600) % 12;
  clockSecAng = (uint16_t)((CLOCK_NOON + sec * 512UL / 60UL) % 512UL);
  clockMinAng =
      (uint16_t)((CLOCK_NOON + (min * 60UL + sec) * 512UL / 3600UL) % 512UL);
  clockHourAng = (uint16_t)(
      (CLOCK_NOON + (hr * 3600UL + min * 60UL + sec) * 512UL / 43200UL) % 512UL);
  clockDirs();
}

// Seconds since midnight, derived rather than stored, so it stays correct
// however long the board has been up.
static uint32_t clockNow(void) {
  // Real time wins as soon as any source has supplied one -- NTP, or the RTC
  // at boot.  `clock rate` and `clock set` drive the free-running fallback
  // below, which is what runs until then, so they stop having an effect once
  // the clock is real.
  uint32_t secOfDay;
  if (timeIsExternal() && timeLocalSecOfDay(secOfDay))
    return secOfDay;

  uint32_t elapsed = ((millis() - clockBaseMs) / 1000UL) * clockRate;
  return (clockBaseSec + elapsed) % 86400UL;
}

#endif // CLOCK


// SETTINGS ----------------------------------------------------------------
// Stored in NVS, which already has a partition, so nothing else is needed.
//
// The eye design is saved by NAME rather than index: indices shift whenever
// the set of EYE_* switches changes, so a saved index could silently select a
// different design after a rebuild.  A name that is no longer built in falls
// back to the first design and says so.

static Preferences prefs;
static bool settingsDirty = false;

#define PREFS_NAMESPACE "creeper"
#define PREFS_KEY_EYE "eye"
#define PREFS_KEY_SWAP "swap"
#define PREFS_KEY_FLIP "flip" // bit 0: SELECT_L_PIN's panel, bit 1: SELECT_R_PIN's
#define PREFS_KEY_PUPIL "pupil"
#define PREFS_KEY_TZ "tz"
#define PREFS_KEY_NTP "ntp"
#define PREFS_KEY_CLK_ON "clkOn"
#define PREFS_KEY_CLK_SEC "clkSec"
#define PREFS_KEY_CLK_RATE "clkRate"
#define PREFS_KEY_CLK_C0 "clkC0"
#define PREFS_KEY_CLK_C1 "clkC1"
#define PREFS_KEY_CLK_C2 "clkC2"
#define PREFS_KEY_SLP_ON "slpOn"
#define PREFS_KEY_SLP_A "slpStart"
#define PREFS_KEY_SLP_B "slpStop"
#define PREFS_KEY_SLP_LVL "slpLevel"
#define PREFS_KEY_DIM "dim"
#define PREFS_KEY_DIM_GAMMA "dimGamma" // tenths: 22 is gamma 2.2
#define PREFS_KEY_DIM_TRIM0 "dimTrim0" // SELECT_L_PIN's panel, -50..50
#define PREFS_KEY_DIM_TRIM1 "dimTrim1" // SELECT_R_PIN's
#define PREFS_KEY_CPU "cpuMHz" // written when it changes, not by save

// The clock's display preferences are saved; the time itself is not.
// Restoring a time from whenever the power went off would be wrong by
// exactly that interval, and NTP or the RTC supplies the real one, which
// makes a stored one pointless as well as misleading.

static void saveSettings(void) {
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putString(PREFS_KEY_EYE, designAt(eyeDesign).name);
  prefs.putBool(PREFS_KEY_SWAP, eyesSwapped);
  prefs.putUChar(PREFS_KEY_FLIP,
                 (uint8_t)((panelFlip[0] ? 1 : 0) | (panelFlip[1] ? 2 : 0)));
  prefs.putBool(PREFS_KEY_PUPIL, pupilOn);
  prefs.putString(PREFS_KEY_TZ, tzString);
#if NETWORK
  prefs.putBool(PREFS_KEY_NTP, netNtpEnabled());
#endif
#if CLOCK
  prefs.putBool(PREFS_KEY_CLK_ON, clockOn);
  prefs.putBool(PREFS_KEY_CLK_SEC, clockSeconds);
  prefs.putUShort(PREFS_KEY_CLK_RATE, clockRate);
  prefs.putULong(PREFS_KEY_CLK_C0, clockRGB[0]);
  prefs.putULong(PREFS_KEY_CLK_C1, clockRGB[1]);
  prefs.putULong(PREFS_KEY_CLK_C2, clockRGB[2]);
  // Not the time -- see the note by the keys.
#endif
#if SLEEP
  prefs.putBool(PREFS_KEY_SLP_ON, sleepEnabled());
  prefs.putUShort(PREFS_KEY_SLP_A, sleepStart());
  prefs.putUShort(PREFS_KEY_SLP_B, sleepStop());
  prefs.putUChar(PREFS_KEY_SLP_LVL, sleepLevel());
#endif
  prefs.putUChar(PREFS_KEY_DIM, dimmerPercent());
  prefs.putUChar(PREFS_KEY_DIM_GAMMA, dimmerGammaX10());
  prefs.putChar(PREFS_KEY_DIM_TRIM0, dimmerTrim(0));
  prefs.putChar(PREFS_KEY_DIM_TRIM1, dimmerTrim(1));
  prefs.end();
  settingsDirty = false;
}

// Wipes the whole namespace, so the build defaults apply at the next boot.
static void forgetSettings(void) {
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
  settingsDirty = false;
  cpuSetting = F_CPU / 1000000; // the build's speed again, from the next boot
}

#if RAM_TABLES
// The two tables drawEye() reads out of order -- the polar map and the iris
// -- copied into RAM whenever the design changes, so a flash cache miss
// cannot cost them; see RAM_TABLES.  Allocated at the first design change and
// kept.  Both or neither: if either allocation fails, both are freed and the
// renderer reads flash, as it always did, until the next design change tries
// again.  The first failure is a warning, since it costs frames; the retries
// are not, or every design change would add another.
static uint16_t *ramPolar = NULL, *ramIris = NULL;
static bool ramTablesNoted = false;
#endif

// Repoints the five artwork pointers at another design.  drawEye() reads them
// per pixel, so this must run between frames or it tears one.  Reached
// through pendingEyeDesign, never directly from an operation -- except
// dropLoadedDesign(), which explains why it cannot wait.  Out-of-range falls
// back to the first design.
static void setEyeDesign(uint8_t idx) {
  if (idx >= designCount())
    idx = 0;
  const EyeDesign *d = &designAt(idx);
  sclera = d->sclera;
  upper = d->upper;
  lower = d->lower;
  polar = d->polar;
  iris = d->iris;
#if RAM_TABLES
  if (!ramPolar) {
    ramPolar = (uint16_t *)malloc(sizeof(uint16_t) * IRIS_WIDTH * IRIS_HEIGHT);
    ramIris = (uint16_t *)malloc(sizeof(uint16_t) * IRIS_MAP_WIDTH * IRIS_MAP_HEIGHT);
    if (!ramPolar || !ramIris) {
      free(ramPolar);
      free(ramIris);
      ramPolar = ramIris = NULL;
      if (!ramTablesNoted)
        healthNote("no memory for the eye tables in RAM; frames are slower");
      ramTablesNoted = true;
    }
  }
  if (ramPolar) {
    memcpy(ramPolar, d->polar, sizeof(uint16_t) * IRIS_WIDTH * IRIS_HEIGHT);
    memcpy(ramIris, d->iris, sizeof(uint16_t) * IRIS_MAP_WIDTH * IRIS_MAP_HEIGHT);
    polar = (const uint16_t(*)[IRIS_WIDTH])ramPolar;
    iris = (const uint16_t(*)[IRIS_MAP_WIDTH])ramIris;
  }
#endif
  eyeDesign = idx;
#if CONTROLLABLE
  settingsDirty = true;
#endif
}

// DISPLAY HARDWARE CONFIG -------------------------------------------------

// Which panel is fitted.  Set from platformio.ini build_flags; the SSD1351
// colour path is the default so the RGB build is unaffected.
//   SSD1351 - Waveshare 1.5" RGB OLED, 65K colour, 16 bits/pixel
//   SSD1327 - Waveshare 1.5" OLED,     16 greys,   4 bits/pixel
#ifndef USE_SSD1327
#define USE_SSD1327 0
#endif

#if USE_SSD1327
#include "ssd1327.h"
typedef SSD1327 displayType;
// Two panels on one bus; drop this if long jumpers make it unreliable.
#ifndef SSD1327_SPI_HZ
#define SSD1327_SPI_HZ 8000000
#endif
static SPISettings graySPI(SSD1327_SPI_HZ, MSBFIRST, SPI_MODE0);
#else
#include <Adafruit_SSD1351.h> // OLED display library -OR-

// Adafruit_SPITFT keeps its chip-select pin in a protected member with no
// setter, so a thin subclass exposes it.  That lets a miswired pair of
// panels be swapped in software rather than rewired -- see `swap`.
class SwappableSSD1351 : public Adafruit_SSD1351 {
public:
  using Adafruit_SSD1351::Adafruit_SSD1351;
  void setCS(int8_t pin) { _cs = pin; }
};

typedef SwappableSSD1351 displayType; // Using OLED display(s)

// The colour panels' bus speed.  A colour frame is 32 KB, four times a
// greyscale one, so this is most of the colour frame rate: at the library's
// default of 8 MHz sending one eye took 33 ms of a 53 ms frame, and the
// frame rate was 19.  16 MHz gives 28-29.  20 MHz, the SSD1351's rated
// maximum, gave 30-31 but speckled one of frank-dev's two panels on the
// carrier board, so the default stays a step below it; both divide the
// ESP32's 80 MHz exactly.  Lower it if long jumpers speckle the image.
#ifndef SSD1351_SPI_HZ
#define SSD1351_SPI_HZ 16000000
#endif
#endif

#define DISPLAY_DC 33    // Data/command pin for BOTH displays
#define DISPLAY_RESET 27 // Reset pin for BOTH displays
// NOTE: these names are the viewer's left/right, not Frank's.  Verified on
// the bench: D15 drives the panel on the viewer's left, which is FRANK'S
// RIGHT eye; D4 drives Frank's left.  Kept as-is to match the README, but
// see showSplash() for the labels that are correct from Frank's side.
#define SELECT_L_PIN 15  // viewer's left  = Frank's RIGHT eye
#define SELECT_R_PIN 04  // viewer's right = Frank's LEFT eye

// INPUT CONFIG (for eye motion -- enable or comment out as needed) --------

// Eyelid tracks the pupil: look down and the upper lid drops with it.
#ifndef TRACKING
#define TRACKING 1
#endif

// Pupil range: IRIS_MIN is the narrowest pupil and IRIS_MAX the widest (see
// dilateCmdActive for why).  Narrowed from upstream's 120/720 so a pair of
// eyes do not look odd beside each other.
#ifndef IRIS_MIN
#define IRIS_MIN 150
#endif
#ifndef IRIS_MAX
#define IRIS_MAX 400
#endif

// Eyes blink on their own.
#ifndef AUTOBLINK
#define AUTOBLINK 1
#endif

// Probably don't need to edit any config below this line, -----------------
// unless building a single-eye project (pendant, etc.), in which case one
// of the two elements in the eye[] array further down can be commented out.

// Eye blinks are a tiny 3-state machine.  Per-eye allows winks + blinks.
#define NOBLINK 0 // Not currently engaged in a blink
#define ENBLINK 1 // Eyelid is currently closing
#define DEBLINK 2 // Eyelid is currently opening
typedef struct {
  uint8_t state;      // NOBLINK/ENBLINK/DEBLINK
  uint32_t duration;  // Duration of blink state (micros)
  uint32_t startTime; // Time (micros) of last state change
} eyeBlink;

#define MOSI_PIN 18
#define MISO_PIN 19
#define SCLK_PIN 5

struct {
  displayType display; // OLED/TFT object
  uint8_t cs;          // Chip select pin
  eyeBlink blink;      // Current blink state
} eye[] = {
#if USE_SSD1327
    {SSD1327(SELECT_L_PIN, DISPLAY_DC), SELECT_L_PIN, {NOBLINK}},
    {SSD1327(SELECT_R_PIN, DISPLAY_DC), SELECT_R_PIN, {NOBLINK}},
#else
    // OK to comment out one of these for single-eye display.
    //
    // NOTE: reset is passed as -1, not DISPLAY_RESET.  The library resets
    // inside begin(), and because both panels share one reset line that
    // would wipe the first panel's init while starting the second.  setup()
    // pulses the shared line once instead.
    {SwappableSSD1351(128, 128, &SPI, SELECT_L_PIN, DISPLAY_DC, -1),
     SELECT_L_PIN,
     {NOBLINK}},
    {SwappableSSD1351(128, 128, &SPI, SELECT_R_PIN, DISPLAY_DC, -1),
     SELECT_R_PIN,
     {NOBLINK}},
#endif
};
#define NUM_EYES (sizeof(eye) / sizeof(eye[0]))

// display.h publishes these for modules that draw text but have no
// business knowing about eye[] or the artwork headers.
uint8_t displayCount(void) { return NUM_EYES; }
static_assert(PANEL_W == SCREEN_WIDTH && PANEL_H == SCREEN_HEIGHT,
              "display.h panel size must match the eye artwork");

// Called between frames only: a swap landing mid-transaction would leave a
// chip select asserted on the wrong panel.
static void applySwap(void) {
  displayQuiesce(); // a frame in flight is using the chip select this moves
  uint8_t a = eye[0].cs, b = eye[1].cs;
  eye[0].cs = b;
  eye[1].cs = a;
  eye[0].display.setCS((int8_t)b);
  eye[1].display.setCS((int8_t)a);
}

// Which panelFlip[] entry belongs to a chip select.
static uint8_t flipSlot(uint8_t cs) { return cs == SELECT_R_PIN ? 1 : 0; }

// Send each panel its orientation.  Between frames only, like applySwap():
// it is one command per panel, but it opens its own SPI transaction, and
// after a swap it must run second so each eye's display object already
// has the chip select it is going to keep.
//
// Done in the controller's remap register rather than by reversing the
// frame, so the eyes, the clock hands, the splash, the address cards and
// the QR code are all covered without any of them knowing.  On the
// SSD1351 the library's own setRotation() sends the same register.
static void applyFlips(void) {
  displayQuiesce(); // setRotation() changes state the sender reads
  for (uint8_t e = 0; e < NUM_EYES; e++) {
    const bool f = panelFlip[flipSlot(eye[e].cs)];
#if USE_SSD1327
    eye[e].display.setFlip(graySPI, f);
#else
    eye[e].display.setRotation(f ? 2 : 0);
#endif
  }
}

// SHARED TEXT RENDERING ----------------------------------------------------
// Used by the startup splash and by the network address cards, so these
// live outside both feature guards.

// The default GFX font is a 6x8 cell, so a string's width is just its
// length scaled up.
void splashCenter(GFXcanvas1 &c, const char *str, uint8_t size,
                         int16_t y) {
  c.setTextSize(size);
  c.setCursor((SCREEN_WIDTH - (int16_t)strlen(str) * 6 * size) / 2, y);
  c.print(str);
}

// PANEL POWER ---------------------------------------------------------------
// Lit or dark, and how bright, without drawing anything.
//
// The dimmer drives these rather than pushing black or scaled pixels.  An
// OLED showing black is already dark, so a frame of black costs a full SPI
// push to achieve what one command does -- and the command also stops the
// panel driving its rows, which a frame of black does not.
//
// The state is tracked here so callers can ask for what they want without
// caring what is already true.  pushCanvas() in particular asks the dimmer
// for readable panels before every card it draws, which is what stops an
// address card or an update message arriving invisibly on a sleeping head.

static bool panelsOn = true;

bool displayIsOn(void) { return panelsOn; }

void displaySetPower(bool on) {
  if (on == panelsOn)
    return;
  panelsOn = on;
  for (uint8_t e = 0; e < NUM_EYES; e++) {
#if USE_SSD1327
    eye[e].display.setPower(graySPI, on);
#else
    // startWrite()/endWrite() arbitrate the bus and this panel's own chip
    // select; each display object carries its own.
    eye[e].display.startWrite();
    eye[e].display.writeCommand(on ? SSD1351_CMD_DISPLAYON
                                   : SSD1351_CMD_DISPLAYOFF);
    eye[e].display.endWrite();
#endif
  }
}

// What each controller's initialisation leaves it at, against the most it can
// drive.  The SSD1327's contrast register goes to 255 and begin() sets 0x80.
// The SSD1351 starts at full master current, with the three colour channels
// at 0xC8, 0x80 and 0xC8 -- blue and red at 200 of 255, green lower to balance
// them.  The dimmer's 100% scales that ratio up until the highest reaches
// 255, so the colour balance never changes, only the level.
#if USE_SSD1327
static const float INITIAL_LUMINANCE = 0x80 / 255.0f;
#else
static const uint8_t CHANNEL_INIT[3] = {0xC8, 0x80, 0xC8};
static const float INITIAL_LUMINANCE = 0xC8 / 255.0f;
#endif

float displayInitialLuminance(void) { return INITIAL_LUMINANCE; }

uint8_t displaySlot(uint8_t e) { return flipSlot(eye[e].cs); }

// The registers last sent to each panel, so a fade that has not moved a
// register this frame sends nothing.  Zero means "not sent yet".
static uint16_t sentLevel[NUM_EYES];

void displaySetLuminance(uint8_t e, float f) {
  if (e >= NUM_EYES)
    return;
  if (f > 1.0f)
    f = 1.0f;
#if USE_SSD1327
  // One register, current in 256 steps.  Floor of 1: above zero means lit.
  long reg = lroundf(f * 255.0f);
  uint8_t contrast = (uint8_t)(reg < 1 ? 1 : reg);
  if (sentLevel[e] == (uint16_t)(contrast + 1))
    return;
  sentLevel[e] = contrast + 1;
  eye[e].display.setContrast(graySPI, contrast);
#else
  // Two stages.  Master current is sixteen coarse steps, (m + 1) / 16 of
  // full; each channel then has 256 fine ones.  Take the smallest master
  // step that reaches the level and make up the rest in the channels, which
  // keeps fine resolution all the way down instead of sixteen jumps.
  int m = (int)ceilf(f * 16.0f) - 1;
  if (m < 0)
    m = 0;
  const float fine = f * 16.0f / (m + 1); // 0..1 of the channels' own range
  uint8_t ch[3];
  for (uint8_t i = 0; i < 3; i++) {
    long v = lroundf(fine * CHANNEL_INIT[i] * (255.0f / 0xC8));
    ch[i] = (uint8_t)(v < 1 ? 1 : v > 255 ? 255 : v);
  }
  // Two bytes of signature is plenty to notice "unchanged": the master step
  // and the brightest channel move whenever anything does.
  const uint16_t sig = (uint16_t)((m << 8) | ch[0]) + 1;
  if (sentLevel[e] == sig)
    return;
  sentLevel[e] = sig;
  eye[e].display.startWrite();
  eye[e].display.writeCommand(SSD1351_CMD_CONTRASTABC);
  for (uint8_t i = 0; i < 3; i++)
    eye[e].display.spiWrite(ch[i]);
  eye[e].display.writeCommand(SSD1351_CMD_CONTRASTMASTER);
  eye[e].display.spiWrite((uint8_t)m);
  eye[e].display.endWrite();
#endif
}

// Push a 1-bit canvas to one panel, in whichever format it wants.  Shared by
// the splash and by the network messages.
void pushCanvas(uint8_t e, GFXcanvas1 &canvas) {
  // An eye frame queued before this card would otherwise land on top of it.
  displayQuiesce();
  // Anything drawing a card wants the panels lit and readable, whatever the
  // dimmer or sleep mode had them at.  Cheap when they already are.
  dimmerCard();

#if USE_SSD1327
  // 1 bit per pixel in, 4 bits per pixel out, two pixels to a byte.
  static uint8_t buf[SSD1327_FRAME_BYTES];
  uint16_t o = 0;
  for (int16_t y = 0; y < SCREEN_HEIGHT; y++)
    for (int16_t x = 0; x < SCREEN_WIDTH; x += 2, o++)
      buf[o] = (uint8_t)((canvas.getPixel(x, y) ? 0xF0 : 0x00) |
                         (canvas.getPixel(x + 1, y) ? 0x0F : 0x00));
  SPI.beginTransaction(graySPI);
  eye[e].display.pushFrame(buf);
  SPI.endTransaction();
#else
  eye[e].display.drawBitmap(0, 0, canvas.getBuffer(), SCREEN_WIDTH,
                            SCREEN_HEIGHT, 0xFFFF, 0x0000);
#endif
}


// INITIALIZATION -- runs once at startup ----------------------------------

#if CONTROLLABLE
static void loadSettings(void); // defined below setup(), with the settings
#endif

static void startSender(void); // defined below setup(), with drawEye()

// The task watchdog's last words.  It watches core 0's idle task, which only
// runs when nothing else on that core wants to, so when it fires the question
// is what kept the core busy -- and the panic that follows prints only what
// was running at that instant, to a serial port nobody may be reading.  That
// instant is a poor witness: WiFi preempts everything for moments at a time,
// and a forced test caught it there with the real culprit spinning beneath.
//
// So core 0 is sampled at every tick into a short ring, and the watchdog's
// hook -- a weak symbol of the core's, called from its interrupt just before
// it aborts -- names the task seen most often in the last 32 ms.  It goes in
// RTC memory, which a reset leaves alone, for reportResetReason() to read at
// the next boot.
#define WDT_SAMPLES 32
static TaskHandle_t core0Seen[WDT_SAMPLES];
static uint8_t core0Next = 0;

// Core 0's idle task, counted from an idle hook -- the place the task
// watchdog is fed from, so "idle ran" and "the watchdog was fed" are one
// fact.  The tick hook turns it into the longest run of ticks core 0 went
// without it since boot: the watchdog's own measure, fatal at 5000.
static volatile uint32_t idle0Runs = 0;
static uint32_t idle0Seen = 0, idle0Since = 0;
static volatile uint32_t idle0MaxGap = 0;
static bool idle0Counted = false; // the hook is in; senderTask() relies on it

static bool IRAM_ATTR countIdle0(void) {
  idle0Runs++;
  return true; // and it may still wait for an interrupt
}

// From core 0's tick interrupt, which runs even while flash is busy: IRAM.
static void IRAM_ATTR sampleCore0(void) {
  core0Seen[core0Next++ % WDT_SAMPLES] = xTaskGetCurrentTaskHandle();
  if (idle0Runs != idle0Seen) {
    idle0Seen = idle0Runs;
    idle0Since = 0;
  } else if (++idle0Since > idle0MaxGap) {
    idle0MaxGap = idle0Since;
  }
}

#define WDT_CLUE_MAGIC 0x57444f47u // "WDOG": the clue below is this boot's
static RTC_NOINIT_ATTR struct {
  uint32_t magic;
  char task[configMAX_TASK_NAME_LEN];
} wdtClue;

extern "C" void esp_task_wdt_isr_user_handler(void) {
  TaskHandle_t busiest = NULL;
  uint8_t most = 0;
  for (uint8_t i = 0; i < WDT_SAMPLES; i++) {
    uint8_t n = 0;
    for (uint8_t j = 0; j < WDT_SAMPLES; j++)
      n += core0Seen[j] == core0Seen[i];
    if (core0Seen[i] && n > most) {
      most = n;
      busiest = core0Seen[i];
    }
  }
  const char *name = busiest ? pcTaskGetName(busiest) : "?";
  uint8_t i = 0;
  for (; i < configMAX_TASK_NAME_LEN - 1 && name[i]; i++)
    wdtClue.task[i] = name[i];
  wdtClue.task[i] = '\0';
  wdtClue.magic = WDT_CLUE_MAGIC;
}

// Why the board last reset, when that was not on purpose.  Power-on, a reset
// button and a restart the firmware asked for -- an update, a WiFi change --
// say nothing.  A crash, a watchdog or a brownout goes through health.h, so
// it reaches the console, the API and the control page rather than only a
// serial line printed before anybody was listening.
static void reportResetReason(void) {
  const bool clue = wdtClue.magic == WDT_CLUE_MAGIC;
  wdtClue.magic = 0; // read once; a later reset must not find it
  const char *why = NULL;
  switch (esp_reset_reason()) {
  case ESP_RST_PANIC:
    why = "a crash";
    break;
  case ESP_RST_TASK_WDT:
    if (clue) {
      healthNote("the board restarted after the task watchdog; "
                 "core 0 was busy with %.15s",
                 wdtClue.task);
      return;
    }
    why = "the task watchdog";
    break;
  case ESP_RST_INT_WDT:
    why = "the interrupt watchdog";
    break;
  case ESP_RST_WDT:
    why = "a watchdog";
    break;
  case ESP_RST_BROWNOUT:
    why = "a brownout: the supply dipped too low";
    break;
  default:
    return;
  }
  healthNote("the board restarted after %s", why);
}

// Four centred lines, drawn once and pushed to whichever panels the caller
// wants.  Used whenever the eyes are not running and the head would otherwise
// sit there dark with no explanation: the setup portal, and OTA progress.  Not
// behind STARTUP_SPLASH -- the boot cards are optional, this is not.  `which`
// of -1 means all of them, which is the usual case; the setup portal passes a
// single index because the other eye is showing a QR code and would be wiped
// by a broadcast.
void showMessageOn(int8_t which, const char *l1, const char *l2,
                   const char *l3, const char *l4) {
  GFXcanvas1 canvas(SCREEN_WIDTH, SCREEN_HEIGHT);
  canvas.fillScreen(0);
  canvas.setTextColor(1);
  if (l1)
    splashCenter(canvas, l1, 2, 14);
  if (l2)
    splashCenter(canvas, l2, 2, 40);
  if (l3)
    splashCenter(canvas, l3, 1, 70);
  if (l4)
    splashCenter(canvas, l4, 1, 86);
  // Not named `eye`: that is the panel array at file scope, and NUM_EYES is
  // sizeof(eye)/sizeof(eye[0]), so a parameter of that name silently turns
  // the panel count into arithmetic on a signed char.
  if (which < 0)
    for (uint8_t e = 0; e < NUM_EYES; e++)
      pushCanvas(e, canvas);
  else if ((uint8_t)which < NUM_EYES)
    pushCanvas((uint8_t)which, canvas);
}

void showMessage(const char *l1, const char *l2, const char *l3,
                 const char *l4) {
  showMessageOn(-1, l1, l2, l3, l4);
}

#if STARTUP_SPLASH

// One frame of the countdown, on both panels, each labelled from both sides:
// Frank's own, the way anatomy is described, and the viewer's.  eye[0] is
// Frank's right eye (see SELECT_L_PIN).  Both are shown because every
// previous attempt to write this down was ambiguous.
static void splashDraw(int8_t remain) {
  // One 1-bit canvas serves both panel types: 2 KB, versus 32 KB for a
  // colour one, and the text is monochrome either way.
  GFXcanvas1 canvas(SCREEN_WIDTH, SCREEN_HEIGHT);
  static const char *const franksSide[2] = {"RIGHT", "LEFT"};
  static const char *const yourSide[2] = {"LEFT", "RIGHT"};
  char digit[2] = {(char)('0' + remain), '\0'};

  for (uint8_t e = 0; e < NUM_EYES; e++) {
    canvas.fillScreen(0);
    canvas.setTextColor(1);
    splashCenter(canvas, "FRANK'S", 2, 6);
    splashCenter(canvas, franksSide[e & 1], 2, 26);
    canvas.drawFastHLine(20, 50, SCREEN_WIDTH - 40, 1);
    splashCenter(canvas, "YOUR", 2, 58);
    splashCenter(canvas, yourSide[e & 1], 2, 78);
    splashCenter(canvas, digit, 3, 100);
    pushCanvas(e, canvas);
  }
}

// Hand a clean screen back to the eyes.
static void splashClear(void) {
  for (uint8_t e = 0; e < NUM_EYES; e++) {
#if USE_SSD1327
    eye[e].display.fill(graySPI, 0x0);
#else
    eye[e].display.fillScreen(0x0000);
#endif
  }
}

// Blocking, and only for setup(): there is no render loop yet to come back
// to, and nothing else is waiting on us.
static void showSplash(void) {
  DEBUG_PRINTF("[creeper-eyes] splash: naming panels for %d s\n",
               SPLASH_SECONDS);
  for (int8_t remain = SPLASH_SECONDS; remain > 0; remain--) {
    splashDraw(remain);
    delay(1000);
  }
  splashClear();
}

// The same cards, asked for while the board is running -- by the `splash`
// command, or POST /api/v1/action.
//
// Not blocking, because blocking here stops everything: the render loop, the
// web server it is polled from, and therefore the very request that asked for
// it.  Measured before this split, POST /action splash took 5.2 s against
// 75-90 ms for every other action, and a GET arriving during one waited 4.9 s
// behind it.  Same deadline-and-poll shape netShow uses for the address cards.
static uint32_t splashUntil = 0;
static int8_t splashShown = -1;

static void splashBegin(void) {
  splashUntil = millis() + (uint32_t)SPLASH_SECONDS * 1000UL;
  splashShown = -1;
}

// True while the splash owns the panels.  Draws only when the digit changes,
// so this costs one comparison on all the frames in between.
static bool splashPoll(void) {
  if (!splashUntil)
    return false;
  int32_t left = (int32_t)(splashUntil - millis());
  if (left <= 0) {
    splashUntil = 0;
    splashClear();
    return false;
  }
  int8_t remain = (int8_t)((left + 999) / 1000);
  if (remain != splashShown) {
    splashShown = remain;
    splashDraw(remain);
  }
  return true;
}

#endif // STARTUP_SPLASH

// Order matters here, more than it looks:
//   - an eye design has to be selected before anything renders, because the
//     artwork pointers start unset
//   - every chip select is parked high before the shared reset is pulsed, so
//     no panel is listening while another is being set up
//   - the shared reset is pulsed exactly once, before either panel is
//     initialised; doing it per panel would wipe the first
//   - stored settings load before the splash, so its name cards reflect a
//     restored panel swap and can be used to check it
void setup(void) {
  uint8_t e;

  setEyeDesign(0); // the pointers start unset now, so pick a design first
#if CLOCK
  for (uint8_t i = 0; i < 3; i++)
    clockSetColor(i, clockRGB[i]);
#endif

  DEBUG_BEGIN();
#if COMMANDS
#if !DEBUG
  Serial.begin(DEBUG_BAUD); // the console needs the port even without DEBUG
#endif
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
#endif
#if DEBUG
  pinMode(DEBUG_LED_PIN, OUTPUT);
#endif
  DEBUG_PRINTF("\n[creeper-eyes] boot: %s rev%u @ %u MHz, heap %u\n",
               ESP.getChipModel(), (unsigned)ESP.getChipRevision(),
               (unsigned)getCpuFrequencyMhz(), (unsigned)ESP.getFreeHeap());
  DEBUG_PRINTF("[creeper-eyes] SPI SCK=%u MISO=%u MOSI=%u | eyes=%u\n",
               SCLK_PIN, MISO_PIN, MOSI_PIN, (unsigned)NUM_EYES);
  reportResetReason();
  idle0Counted = // see idle0Runs
      esp_register_freertos_idle_hook_for_cpu(countIdle0, 0) == ESP_OK;
  esp_register_freertos_tick_hook_for_cpu(sampleCore0, 0); // see wdtClue
  randomSeed(analogRead(A3)); // Seed random() from floating analog input

  // Route the SPI bus to the pins in the README wiring table.  Required on
  // generic ESP32 boards, whose variant defaults are MOSI=23/SCK=18.
  SPI.begin(SCLK_PIN, MISO_PIN, MOSI_PIN);

  // Park every chip select before touching the bus, so no panel listens
  // while another is being set up.
  for (e = 0; e < NUM_EYES; e++) {
    pinMode(eye[e].cs, OUTPUT);
    digitalWrite(eye[e].cs, HIGH);
  }

  // Both panels share one reset line, so it is pulsed exactly once, here,
  // before any panel is initialised.
  pinMode(DISPLAY_RESET, OUTPUT);
  digitalWrite(DISPLAY_RESET, HIGH);
  delay(20);
  digitalWrite(DISPLAY_RESET, LOW);
  delay(20);
  digitalWrite(DISPLAY_RESET, HIGH);
  delay(200);

  for (e = 0; e < NUM_EYES; e++) {
#if USE_SSD1327
    eye[e].display.begin(graySPI);
    eye[e].display.fill(graySPI, 0x0);
#else
    digitalWrite(eye[e].cs, LOW); // Select one eye for init
    eye[e].display.begin(SSD1351_SPI_HZ); // kept for every transfer after
    digitalWrite(eye[e].cs, HIGH); // Deselect
#endif
  }
  DEBUG_PRINTF("[creeper-eyes] %u panel(s) initialised (%s)\n",
               (unsigned)NUM_EYES, USE_SSD1327 ? "SSD1327 grey" : "SSD1351 rgb");

  // Eyelid mirroring for the left eye is done in software, in drawEye(), so
  // it behaves the same on both panel types.  The hardware alternative,
  // SSD1351_CMD_SETREMAP, mirrors the whole panel in its controller -- which
  // would also mirror gaze direction and cross the eyes.

  timeApplyTz(); // the built-in default, until settings say otherwise
  dimmerBegin(); // the panels are up at their initial level; start from it

#if CONTROLLABLE
  eyeStoreBegin(); // before loadSettings(), which may name the loaded design
  adoptLoadedDesign();
#endif
#if CONTROLLABLE
  loadSettings(); // before the splash, so its labels are correct
#endif
#if RTC
  rtcBegin(); // before the network: a sync later simply outranks it
#endif
#if STARTUP_SPLASH
  showSplash(); // before the network: that can take a while, and a dark head
                // looks broken rather than busy
#endif
#if NETWORK
  setupNetwork();
  netOnConnected();
#endif
  startSender(); // before the render loop, which is the first to use it
#if COMMANDS
  Serial.println(F("[creeper-eyes] console ready -- type 'help'"));
#endif
}

// SENDING FRAMES -----------------------------------------------------------
//
// With OVERLAP_SEND, a finished frame is handed to a small task on core 0,
// which sends it while this core -- the render loop, on core 1 -- draws the
// next into the other buffer.  Handing a frame over waits for the previous
// one to be off the wire, so a buffer is never drawn into while it is being
// sent.
//
// The two cores share the SPI bus safely without anything here: the Arduino
// core's beginTransaction() takes a mutex and endTransaction() gives it
// back, and every panel write -- the Adafruit library's, the SSD1327
// class's, a frame -- is one transaction.  A command from the render loop
// simply waits while a frame goes out.  What the mutex cannot do is order
// things, which is what displayQuiesce() is for.
//
// Both rest on assumptions worth stating.  The mutex is the Arduino core's,
// there only while its HAL locks are built in -- the default, checked below.
// And displayQuiesce() orders things only because the render loop is the one
// task that draws: it queues every frame and draws every card, so once it
// has waited out the frame in flight, nothing can be queued before its card.
// Drawing from another task -- a web server moved into its own, say -- would
// need a lock around the wait and the card together.
#if OVERLAP_SEND && CONFIG_DISABLE_HAL_LOCKS
#error "OVERLAP_SEND needs the SPI bus lock, which CONFIG_DISABLE_HAL_LOCKS removes"
#endif

// Where a frame's time goes, summed over the heartbeat's interval and
// reported with it: drawing the pixels, waiting for the previous frame to be
// sent (with OVERLAP_SEND), and sending -- on core 0, or here without it.
// The rest of the interval is everything else the render loop does.
static uint32_t perfDrawUs = 0, perfWaitUs = 0, perfSendUs = 0, perfEyeUs = 0;
static portMUX_TYPE perfMux = portMUX_INITIALIZER_UNLOCKED; // perfSendUs, cross-core

// Sends one finished frame to eye e's panel, on whichever core calls it.
static void sendFrame(uint8_t e, const void *frame) {
#if USE_SSD1327
  SPI.beginTransaction(graySPI);
  eye[e].display.pushFrame((const uint8_t *)frame);
  SPI.endTransaction();
#else
  eye[e].display.startWrite();
  // The library's own window, which sends each bound as the one byte the
  // controller takes.  Not by hand as 16-bit writes: that sends four bytes
  // where two are expected, and the frames never land.
  eye[e].display.setAddrWindow(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
  // One large burst, through the ESP32's SPI FIFO.
  SPI.writePixels(frame, SCREEN_WIDTH * SCREEN_HEIGHT * sizeof(uint16_t));
  eye[e].display.endWrite();
#endif
}

static void addSendTime(uint32_t us) {
  portENTER_CRITICAL(&perfMux);
  perfSendUs += us;
  portEXIT_CRITICAL(&perfMux);
}

#if OVERLAP_SEND
struct SendJob {
  uint8_t e;
  const void *frame;
};
static QueueHandle_t sendJobs = NULL;     // one job at most
static SemaphoreHandle_t sendIdle = NULL; // available while nothing is in hand

// Sending busy-waits on the SPI FIFO, and the render loop always has the
// next frame ready, so this would keep core 0 busy without a break.  Core
// 0's idle task would then never run, and the task watchdog, which listens
// for it, aborts the board: it did, after about 25 seconds.  Dropping to the
// idle task's own priority halved the send rate instead, because the idle
// task takes its whole tick.  The watchdog only needs the idle task to run
// once in its five seconds, so the sender steps aside once a second: about a
// thousandth of the time.
//
// Stepping aside for one tick is not always enough.  Anything else on core 0
// that wants that tick gets it first -- WiFi, the TCP/IP stack, mDNS at the
// sender's own priority -- and on colour, where the sender never waits for
// work, those rests are nearly the idle task's only chance.  On frank-dev,
// idle on a quiet network, about one rest in twelve went to something else,
// two in a row within six minutes, and five in a row is the watchdog: it
// fired twice in three days.  So the sender rests until the idle task has
// actually run, a tick at a time.  It gives up after
// SENDER_REST_MAX_TICKS, because by then core 0 is being kept busy by
// something other than the sender, which resting longer would not help.
#define SENDER_REST_MS 1000
#define SENDER_REST_MAX_TICKS 50

// Rests taken, and rests whose first tick went to something else, so that
// the sender had to wait longer: for `status`.
static volatile uint32_t senderRests = 0, senderRestsMissed = 0;

static void senderTask(void *) {
  SendJob j;
  uint32_t lastRest = millis();
  for (;;) {
    xQueueReceive(sendJobs, &j, portMAX_DELAY);
    const uint32_t t0 = micros();
    sendFrame(j.e, j.frame);
    addSendTime(micros() - t0);
    xSemaphoreGive(sendIdle);
    if (millis() - lastRest >= SENDER_REST_MS) {
      const uint32_t idleBefore = idle0Runs;
      uint32_t extra = 0;
      do {
        vTaskDelay(1); // blocked, so core 0's idle task can have its turn
      } while (idle0Counted && idle0Runs == idleBefore &&
               ++extra < SENDER_REST_MAX_TICKS);
      lastRest = millis();
      senderRests++;
      if (extra)
        senderRestsMissed++;
    }
  }
}
#endif

void displayQuiesce(void) {
#if OVERLAP_SEND
  if (!sendIdle)
    return;
  xSemaphoreTake(sendIdle, portMAX_DELAY);
  xSemaphoreGive(sendIdle);
#endif
}

// Hands a finished frame over, or sends it here without OVERLAP_SEND.
static void submitFrame(uint8_t e, const void *frame) {
#if OVERLAP_SEND
  if (sendJobs) {
    const uint32_t t0 = micros();
    xSemaphoreTake(sendIdle, portMAX_DELAY); // the previous frame is sent
    perfWaitUs += micros() - t0;
    const SendJob j = {e, frame};
    xQueueSend(sendJobs, &j, portMAX_DELAY);
    return;
  }
#endif
  const uint32_t t0 = micros();
  sendFrame(e, frame);
  addSendTime(micros() - t0);
}

// Last thing in setup(), once the panels are up and before the render loop
// starts.  Priority 1 on core 0, below WiFi and the TCP/IP stack, which
// preempt it: a frame paused mid-send loses nothing, the chip select simply
// stays low a little longer.  See senderTask() for why it rests.
static void startSender(void) {
#if OVERLAP_SEND
  sendIdle = xSemaphoreCreateBinary();
  sendJobs = xQueueCreate(1, sizeof(SendJob));
  if (!sendIdle || !sendJobs ||
      xTaskCreatePinnedToCore(senderTask, "panels", 3072, NULL, 1, NULL, 0) !=
          pdPASS) {
    // Without the task, frames are sent from the render loop as before.
    healthNote("the frame sender could not start; frames are slower");
    if (sendJobs)
      vQueueDelete(sendJobs);
    if (sendIdle)
      vSemaphoreDelete(sendIdle);
    sendJobs = NULL;
    sendIdle = NULL;
    return;
  }
  xSemaphoreGive(sendIdle);
#endif
}

// EYE RENDERING ------------------------------------------------------------

// The frame buffers: two with OVERLAP_SEND, so one can be drawn while the
// other is sent.  Colour frames go out as drawn; greyscale ones are drawn in
// RGB565 like colour, then packed to four bits a pixel, and it is the packed
// frame that is sent -- so there, only the packed buffer needs a second copy.
// drawEye() draws into pBurst either way: on greyscale it is the one scratch
// buffer below, on colour a pointer to whichever colourFrames buffer is free,
// so the drawing code is the same for both.
#define FRAME_BUFFERS (OVERLAP_SEND ? 2 : 1)
#if USE_SSD1327
static uint16_t pBurst[SCREEN_WIDTH * SCREEN_HEIGHT];
static uint8_t gBurst[FRAME_BUFFERS][SSD1327_FRAME_BYTES];
#else
static uint16_t colourFrames[FRAME_BUFFERS][SCREEN_WIDTH * SCREEN_HEIGHT];
#endif
static uint8_t nextBuffer = 0;

void drawEye(        // Renders one eye.  Inputs must be pre-clipped & valid.
    uint8_t e,       // Eye array index; 0 is Frank's right (viewer's left)
    uint32_t iScale, // Scale factor for iris
    uint8_t scleraX, // First pixel X offset into sclera image
    uint8_t scleraY, // First pixel Y offset into sclera image
    uint8_t uT,      // Upper eyelid threshold value
    uint8_t lT) {    // Lower eyelid threshold value

  uint8_t screenX, screenY, scleraXsave;
  int16_t irisX, irisY;
  uint16_t p, a;
  uint32_t d;

  // The left eye's EYELIDS are mirrored so the pair reads as a matched set,
  // tear ducts inboard, the way real eyes are shaped.
  //
  // The eyeball is deliberately NOT mirrored.  Gaze direction comes from
  // panning the window into the sclera and iris, so mirroring that too makes
  // the pupils pan in opposite directions and the eyes go cross-eyed.
  // Eyelids mirror; gaze does not.
  const bool mirrorLids = (e == 0);
  const uint32_t tStart = micros();
  const uint8_t buf = nextBuffer;
  nextBuffer = (uint8_t)((nextBuffer + 1) % FRAME_BUFFERS);
#if !USE_SSD1327
  uint16_t *const pBurst = colourFrames[buf]; // the frame itself, sent as drawn
#endif

  scleraXsave = scleraX; // Save initial X value to reset on each line
  irisY = scleraY - (SCLERA_HEIGHT - IRIS_HEIGHT) / 2;
#if CLOCK
  // Where the iris sits in screen coordinates, for the clock hands below.
  const int16_t irisOriginX = (int16_t)scleraXsave - (SCLERA_WIDTH - IRIS_WIDTH) / 2;
  const int16_t irisOriginY = irisY;
#endif
  for (screenY = 0; screenY < SCREEN_HEIGHT; screenY++, scleraY++, irisY++) {
    scleraX = scleraXsave;
    irisX = scleraXsave - (SCLERA_WIDTH - IRIS_WIDTH) / 2;
    for (screenX = 0; screenX < SCREEN_WIDTH; screenX++, scleraX++, irisX++) {
      // SCREEN_WIDTH - 1 - screenX, not SCREEN_WIDTH - screenX: the latter
      // yields 128 at screenX == 0, one past the end of the row.
      const uint8_t lidX =
          mirrorLids ? (uint8_t)(SCREEN_WIDTH - 1 - screenX) : screenX;
      if ((lower[screenY][lidX] <= lT) ||
          (upper[screenY][lidX] <= uT)) { // Covered by eyelid
        p = 0;
      } else if ((irisY < 0) || (irisY >= IRIS_HEIGHT) || (irisX < 0) ||
                 (irisX >= IRIS_WIDTH)) { // In sclera
        p = sclera[scleraY][scleraX];
      } else {                                   // Maybe iris...
        p = polar[irisY][irisX];                 // Polar angle/dist
        d = (iScale * (p & 0x7F)) / 128;         // Distance (Y)
        if (d < IRIS_MAP_HEIGHT) {               // Within iris area
          a = (IRIS_MAP_WIDTH * (p >> 7)) / 512; // Angle (X)
          p = iris[d][a];                        // Pixel = iris
        } else {                                 // Not in iris
          p = sclera[scleraY][scleraX];          // Pixel = sclera
        }
      }
      pBurst[screenY * SCREEN_WIDTH + screenX] = p;
    }
  }

#if CLOCK
  // Hands are drawn after the eye, straight into the finished frame, so they
  // are real line segments of constant width rather than the pie wedges that
  // came out of testing a fixed angular spread per pixel.  It is also much
  // cheaper: a few hundred pixels instead of a test against all 16384.
  //
  // Being outside the pixel loop means the two clips it provided have to be
  // repeated here -- the iris circle, and the eyelids.
  // Suppressed rather than switched off when the time is unknown: with no
  // network, no RTC and nothing typed in, a clock face is a confident lie.
  if (clockOn && timeSynced) {
    const uint8_t hlen[3] = {CLOCK_HOUR_LEN, CLOCK_MIN_LEN, CLOCK_SEC_LEN};
    const uint8_t hhw[3] = {CLOCK_HOUR_HW, CLOCK_MIN_HW, CLOCK_SEC_HW};
    const int16_t cx = (int16_t)(IRIS_WIDTH / 2) - irisOriginX;
    const int16_t cy = (int16_t)(IRIS_HEIGHT / 2) - irisOriginY;

    // Hour first so the minute and second hands lie over it.
    for (uint8_t h = 0; h < 3; h++) {
      if (h == 2 && !clockSeconds)
        continue;

      const int32_t ux = clockDirX[h], uy = clockDirY[h]; // 8.8 unit vector
      const int32_t len8 = (int32_t)hlen[h] << 8;
      // Half a pixel of slack, so a nominal half-width of 0 still draws a
      // one-pixel line rather than nothing.
      const int32_t halfW8 = ((int32_t)hhw[h] << 8) + 128;

      // Tight box around the hand, padded by the half-width, then clipped.
      const int16_t tx = cx + (int16_t)((ux * (int32_t)hlen[h]) >> 8);
      const int16_t ty = cy + (int16_t)((uy * (int32_t)hlen[h]) >> 8);
      const int16_t pad = (int16_t)hhw[h] + 1;
      int16_t x0 = (cx < tx ? cx : tx) - pad, x1 = (cx > tx ? cx : tx) + pad;
      int16_t y0 = (cy < ty ? cy : ty) - pad, y1 = (cy > ty ? cy : ty) + pad;
      if (x0 < 0) x0 = 0;
      if (y0 < 0) y0 = 0;
      if (x1 > SCREEN_WIDTH - 1) x1 = SCREEN_WIDTH - 1;
      if (y1 > SCREEN_HEIGHT - 1) y1 = SCREEN_HEIGHT - 1;

      // Every pixel in the box is decided exactly once, which is what keeps
      // the edges clean: walking parallel offset lines instead rounds each
      // one separately and leaves gaps and doubled pixels between them.
      for (int16_t sy = y0; sy <= y1; sy++) {
        const int32_t dy = (int32_t)sy - cy;
        for (int16_t sx = x0; sx <= x1; sx++) {
          const int32_t dx = (int32_t)sx - cx;

          const int32_t along = dx * ux + dy * uy; // 8.8 px along the hand
          if (along < 0 || along > len8)
            continue;
          const int32_t perp = dy * ux - dx * uy; // 8.8 px across it
          if (perp < -halfW8 || perp > halfW8)
            continue;

          const int16_t ix = irisOriginX + sx, iy = irisOriginY + sy;
          if (ix < 0 || ix >= IRIS_WIDTH || iy < 0 || iy >= IRIS_HEIGHT)
            continue;
          if ((polar[iy][ix] & 0x7F) >= 127)
            continue; // outside the iris circle
          const uint8_t lx =
              mirrorLids ? (uint8_t)(SCREEN_WIDTH - 1 - sx) : (uint8_t)sx;
          if (lower[sy][lx] <= lT || upper[sy][lx] <= uT)
            continue; // under an eyelid
          pBurst[sy * SCREEN_WIDTH + sx] = clockPix[h];
        }
      }
    }
  }
#endif

#if USE_SSD1327
  // Pack the RGB565 frame down to 4-bit grey, two pixels per byte.  This
  // halves what goes over the wire compared with the colour panel.
  uint8_t *const packed = gBurst[buf];
  for (uint16_t i = 0, o = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i += 2, o++)
    packed[o] = (uint8_t)((rgb565ToGray4(pBurst[i]) << 4) |
                          rgb565ToGray4(pBurst[i + 1]));
  const void *const frame = packed;
#else
  const void *const frame = pBurst;
#endif
  const uint32_t tDrawn = micros();
  perfDrawUs += tDrawn - tStart;
  submitFrame(e, frame);
  perfEyeUs += micros() - tStart;
}

// EYE ANIMATION -----------------------------------------------------------

const uint8_t ease[] = { // Ease in/out curve for eye movements 3*t^2-2*t^3
    0,   0,   0,   0,   0,   0,   0,   1,
    1,   1,   1,   1,   2,   2,   2,   3, // T
    3,   3,   4,   4,   4,   5,   5,   6,
    6,   7,   7,   8,   9,   9,   10,  10, // h
    11,  12,  12,  13,  14,  15,  15,  16,
    17,  18,  18,  19,  20,  21,  22,  23, // x
    24,  25,  26,  27,  27,  28,  29,  30,
    31,  33,  34,  35,  36,  37,  38,  39, // 2
    40,  41,  42,  44,  45,  46,  47,  48,
    50,  51,  52,  53,  54,  56,  57,  58, // A
    60,  61,  62,  63,  65,  66,  67,  69,
    70,  72,  73,  74,  76,  77,  78,  80, // l
    81,  83,  84,  85,  87,  88,  90,  91,
    93,  94,  96,  97,  98,  100, 101, 103, // e
    104, 106, 107, 109, 110, 112, 113, 115,
    116, 118, 119, 121, 122, 124, 125, 127, // c
    128, 130, 131, 133, 134, 136, 137, 139,
    140, 142, 143, 145, 146, 148, 149, 151, // J
    152, 154, 155, 157, 158, 159, 161, 162,
    164, 165, 167, 168, 170, 171, 172, 174, // a
    175, 177, 178, 179, 181, 182, 183, 185,
    186, 188, 189, 190, 192, 193, 194, 195, // c
    197, 198, 199, 201, 202, 203, 204, 205,
    207, 208, 209, 210, 211, 213, 214, 215, // o
    216, 217, 218, 219, 220, 221, 222, 224,
    225, 226, 227, 228, 228, 229, 230, 231, // b
    232, 233, 234, 235, 236, 237, 237, 238,
    239, 240, 240, 241, 242, 243, 243, 244, // s
    245, 245, 246, 246, 247, 248, 248, 249,
    249, 250, 250, 251, 251, 251, 252, 252, // o
    252, 253, 253, 253, 254, 254, 254, 254,
    254, 255, 255, 255, 255, 255, 255, 255}; // n

#if AUTOBLINK
uint32_t timeOfLastBlink = 0L, timeToNextBlink = 0L; // micros()
#endif

#if CONTROLLABLE

// Returns designCount() if there is no match.
static uint8_t eyeDesignByName(const char *name) {
  for (uint8_t i = 0; i < designCount(); i++)
    if (!strcmp(name, designAt(i).name))
      return i;
  return designCount();
}

// Runs before the splash, so the labels reflect a restored swap.
//
// Nothing read here is trusted: each value goes through nvsread.h, which
// checks its type and range, and a bad one is reported on the control page,
// removed, and replaced by the built-in default.  Opened read-write for that
// removal, and for the CPU speed's brownout fallback below -- nothing else is
// written.
static void loadSettings(void) {
  if (!prefs.begin(PREFS_NAMESPACE, false)) {
    healthNote("settings storage could not be opened; using the defaults");
    return;
  }

  char saved[EYE_FILE_NAME_MAX + 1];
  nvsReadStr(prefs, PREFS_KEY_EYE, saved, sizeof(saved));
  bool sw = nvsReadBool(prefs, PREFS_KEY_SWAP, false);
  uint8_t flip = nvsReadU8(prefs, PREFS_KEY_FLIP, 0, 0, 3); // two bits
  pupilOn = nvsReadBool(prefs, PREFS_KEY_PUPIL, pupilOn);

  char tz[TZ_MAX];
  if (nvsReadStr(prefs, PREFS_KEY_TZ, tz, sizeof(tz))) {
    if (timeTzValid(tz))
      memcpy(tzString, tz, sizeof(tzString));
    else
      nvsReject(prefs, PREFS_KEY_TZ, "is not a timezone");
  }
  timeApplyTz(); // the restored zone, before anything reads a clock

#if NETWORK
  // Restored before setupNetwork runs, so a board saved with it off never
  // asks a server in the first place.
  netNtpSetEnabled(nvsReadBool(prefs, PREFS_KEY_NTP, true));
#endif
#if CLOCK
  clockOn = nvsReadBool(prefs, PREFS_KEY_CLK_ON, clockOn);
  clockSeconds = nvsReadBool(prefs, PREFS_KEY_CLK_SEC, clockSeconds);
  clockRate = nvsReadU16(prefs, PREFS_KEY_CLK_RATE, clockRate, 1, 3600);
  uint32_t c0 = nvsReadU32(prefs, PREFS_KEY_CLK_C0, clockRGB[0], 0, 0xFFFFFF);
  uint32_t c1 = nvsReadU32(prefs, PREFS_KEY_CLK_C1, clockRGB[1], 0, 0xFFFFFF);
  uint32_t c2 = nvsReadU32(prefs, PREFS_KEY_CLK_C2, clockRGB[2], 0, 0xFFFFFF);
#endif
#if SLEEP
  sleepLoad(nvsReadBool(prefs, PREFS_KEY_SLP_ON, SLEEP_ENABLED),
            nvsReadU16(prefs, PREFS_KEY_SLP_A, SLEEP_START_MIN, 0, 1439),
            nvsReadU16(prefs, PREFS_KEY_SLP_B, SLEEP_STOP_MIN, 0, 1439),
            nvsReadU8(prefs, PREFS_KEY_SLP_LVL, SLEEP_LEVEL, 0, 100));
#endif
  // Nothing saved means the brightness the panels always had.
  const int8_t trims[2] = {nvsReadI8(prefs, PREFS_KEY_DIM_TRIM0, 0, -50, 50),
                           nvsReadI8(prefs, PREFS_KEY_DIM_TRIM1, 0, -50, 50)};
  dimmerLoad(nvsReadU8(prefs, PREFS_KEY_DIM, dimmerDefaultPercent(), 0, 100),
             nvsReadU8(prefs, PREFS_KEY_DIM_GAMMA, DIM_GAMMA_X10, DIM_GAMMA_MIN,
                       DIM_GAMMA_MAX),
             trims);

  // nvsReadU8 checks the span; only its two ends are speeds.  And a board
  // that restarted after a brownout comes back at 160 and keeps it: at 240
  // both cores flat out draw enough to sag a marginal supply, and a head
  // sealed in a prop should brown out once, not every time it is powered up.
  uint8_t mhz = nvsReadU8(prefs, PREFS_KEY_CPU, cpuSetting, 160, 240);
  if (!cpuValid(mhz)) {
    nvsReject(prefs, PREFS_KEY_CPU, "is not 160 or 240");
    mhz = cpuSetting;
  }
  if (mhz > 160 && esp_reset_reason() == ESP_RST_BROWNOUT) {
    mhz = 160;
    prefs.putUChar(PREFS_KEY_CPU, mhz);
    healthNote("dropped to 160 MHz after the brownout; set 240 again to retry");
  }
  cpuSetting = mhz;
  prefs.end();
  if (getCpuFrequencyMhz() != cpuSetting) // before WiFi starts: see cpuSetting
    setCpuFrequencyMhz(cpuSetting);

#if CLOCK
  clockSetColor(0, c0);
  clockSetColor(1, c1);
  clockSetColor(2, c2);
  // The time starts at its built-in default; only the hand angles need
  // settling before the first frame.
  clockUpdate();
#endif

  if (sw) {
    eyesSwapped = true;
    applySwap();
  }
  panelFlip[0] = flip & 1;
  panelFlip[1] = flip & 2;
  applyFlips(); // after the swap, so each command reaches its own panel
  if (saved[0]) {
    uint8_t idx = eyeDesignByName(saved);
    if (idx < designCount())
      setEyeDesign(idx);
    else
      // Not damage, so not removed: the design may be a loaded one whose
      // file is gone, and loading it again should bring the setting back.
      healthNote("saved eye '%s' is not on this board; showing %s", saved,
                 designAt(0).name);
  }
  // setEyeDesign() during setup() flags a change; nothing has actually been
  // modified since the store was read, so start clean.
  settingsDirty = false;

  DEBUG_PRINTF("[creeper-eyes] settings: eye=%s swap=%s flip=%s%s pupil=%s "
               "cpu=%u MHz\n",
               designAt(eyeDesign).name, eyesSwapped ? "yes" : "no",
               panelFlip[0] ? "L" : "-", panelFlip[1] ? "R" : "-",
               pupilOn ? "on" : "off", (unsigned)getCpuFrequencyMhz());
#if CLOCK
  DEBUG_PRINTF("[creeper-eyes] clock: %s rate=%ux seconds=%s (time not restored)\n",
               clockOn ? "on" : "off", (unsigned)clockRate,
               clockSeconds ? "on" : "off");
#endif
}

// Numbered listing with the current design marked, so `eye <index>` has
// something to refer to.
static void listEyeDesigns(Print &out) {
  for (uint8_t i = 0; i < designCount(); i++)
    out.printf("  %u  %-10s%s%s\n", (unsigned)i, designAt(i).name,
                  i == eyeDesign ? "  <- current" : "",
                  i == LOADED_DESIGN ? "  (loaded from a file)" : "");
  if (!eyeStoreAvailable())
    out.println(F("  no eye slot on this board; one USB flash adds it"));
  else if (!loadedPresent)
    out.println(F("  eye slot empty; load a file from the control page"));
}

// Gaze override.  Consumed in frame(), which sets the vestigial serEyeCtrl
// flag from it -- that flag is the one piece of the original UART command
// plumbing still wired into the motion state machine.
static bool gazeCmdActive = false;
static bool gazeCmdPending = false;
static int16_t gazeCmdX = 512, gazeCmdY = 512;
static uint16_t lastFps = 0;

// Pupil dilation override.  loop() walks the iris scale randomly between
// IRIS_MIN and IRIS_MAX; while this is active frame() ignores that walk.
//
// The scale divides into the iris map: a larger value pushes pixels out of
// the iris sooner, so IRIS_MAX is the WIDEST pupil and IRIS_MIN the
// narrowest.  The command takes a plain percentage, 100 = fully dilated.
static bool dilateCmdActive = false;
static uint8_t dilateCmdPct = 50;
static uint16_t dilateCmdValue = (IRIS_MIN + IRIS_MAX) / 2;

// Easing state lives at file scope so the startle effect can retune the
// rate, and snap the pupil open instantly when it wants to.
static int32_t dilateCurrent = (IRIS_MIN + IRIS_MAX) / 2;
static uint8_t dilateEaseDiv = 8; // larger = slower approach

// Percentage in, internal scale out.  The mapping is not a straight scale:
// see the note above about IRIS_MAX being the wide end.
static void setDilation(uint8_t pct) {
  if (pct > 100)
    pct = 100;
  dilateCmdPct = pct;
  dilateCmdValue =
      (uint16_t)(IRIS_MIN + ((uint32_t)pct * (IRIS_MAX - IRIS_MIN)) / 100);
  dilateCmdActive = true;
}

// STARTLE -----------------------------------------------------------------
// Squeeze the pupil down slowly, then blow it wide open with a blink.  Run
// as a state machine polled once per frame rather than with delay(), which
// would freeze rendering for the duration of the effect.

enum { STARTLE_OFF, STARTLE_WINDUP, STARTLE_HOLD };
static uint8_t startleState = STARTLE_OFF;
static uint32_t startleMark = 0;
static bool startleWasAuto = true;
static uint8_t startleWasPct = 50;

#ifndef STARTLE_WINDUP_MS
#define STARTLE_WINDUP_MS 1400 // slow constrict -- the tension
#endif
#ifndef STARTLE_HOLD_MS
#define STARTLE_HOLD_MS 1200 // eyes held wide after the jolt
#endif

// Captures whatever dilation state was in effect so it can be handed back
// when the effect finishes.
static void startleBegin(void) {
  startleWasAuto = !dilateCmdActive; // so we can hand back what we took
  startleWasPct = dilateCmdPct;
  setDilation(0);      // constrict to a pinpoint
  dilateEaseDiv = 48;  // ...slowly
  startleMark = millis();
  startleState = STARTLE_WINDUP;
}

// Abandons a running effect and restores the normal easing rate, which the
// windup slows right down.
static void startleCancel(void) {
  startleState = STARTLE_OFF;
  dilateEaseDiv = 8;
}

// Advances the effect one step.  A state machine rather than delay() so the
// eyes keep rendering throughout.
static void pollStartle(void) {
  if (startleState == STARTLE_OFF)
    return;

  uint32_t now = millis();

  if (startleState == STARTLE_WINDUP) {
    if (now - startleMark >= STARTLE_WINDUP_MS) {
      setDilation(100);            // full open
      dilateCurrent = dilateCmdValue; // ...instantly, no ease
      dilateEaseDiv = 8;
#if AUTOBLINK
      timeToNextBlink = 0; // flinch
#endif
      startleMark = now;
      startleState = STARTLE_HOLD;
    }
  } else if (startleState == STARTLE_HOLD) {
    if (now - startleMark >= STARTLE_HOLD_MS) {
      if (startleWasAuto)
        dilateCmdActive = false;
      else
        setDilation(startleWasPct);
      startleState = STARTLE_OFF;
      Serial.println(F("ok startle complete"));
    }
  }
}


// CROSSING THREADS ----------------------------------------------------------
// See the note in state.h.  A recursive mutex because a few operations call
// each other -- stateSetDilation cancels a startle, which is itself an
// operation -- and a plain one would deadlock on the second take.

static SemaphoreHandle_t stateMutex = NULL;

void stateLock(void) {
  // Created on first use rather than in setup(), so an operation arriving
  // before setup() finishes cannot find a null handle.  Single-threaded at
  // that point, so the check is not itself a race.
  if (!stateMutex)
    stateMutex = xSemaphoreCreateRecursiveMutex();
  xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY);
}

void stateUnlock(void) { xSemaphoreGiveRecursive(stateMutex); }

// A scope guard, so an early return cannot leave the lock held.
namespace {
struct StateGuard {
  StateGuard() { stateLock(); }
  ~StateGuard() { stateUnlock(); }
};
} // namespace
#define LOCKED StateGuard _guard

// Queued for the renderer.  -1 means nothing pending.
static int16_t pendingEyeDesign = -1;

// Applied between frames, from the render loop and nowhere else.  Short
// enough that holding the lock across it costs the renderer nothing, and
// taking it once for the whole batch is cheaper than once per item.
void statePollPending(void) {
  if (pendingEyeDesign < 0)
    return; // the common case, and it costs one comparison
  LOCKED;
  if (pendingEyeDesign >= 0) { // re-checked now the lock is held
    setEyeDesign((uint8_t)pendingEyeDesign);
    pendingEyeDesign = -1;
  }
}

// DEVICE OPERATIONS ---------------------------------------------------------
// The implementation of state.h.  Thin by design: each of these does one
// thing to the device and reports whether it worked, leaving every decision
// about wording, status codes and formatting to the caller.

void stateGet(DeviceState &o) {
  LOCKED;
  // The requested design, not the one currently on screen: they differ for at
  // most a frame, and a client that just set one should be told what it set.
  uint8_t shown = pendingEyeDesign >= 0 ? (uint8_t)pendingEyeDesign : eyeDesign;
  o.eyeIndex = shown;
  o.eyeCount = designCount();
  o.eyeName = designAt(shown).name;

  o.gazeManual = gazeCmdActive;
  o.gazeX = gazeCmdX;
  o.gazeY = gazeCmdY;

  o.dilateManual = dilateCmdActive;
  o.dilatePercent = dilateCmdPct;
  o.pupilOn = pupilOn;

  o.swapped = eyesSwapped;
  // Reported per eye as displayed, which is what a person can point at.
  for (uint8_t e = 0; e < 2; e++)
    o.flipped[e] = e < NUM_EYES && panelFlip[flipSlot(eye[e].cs)];
  o.dimPercent = dimmerPercent();
  o.dimShown = dimmerShown();
  o.dimGammaX10 = dimmerGammaX10();
  for (uint8_t e = 0; e < 2; e++)
    o.dimTrim[e] = e < NUM_EYES ? dimmerTrim(flipSlot(eye[e].cs)) : 0;
  o.dimSweeping = dimmerSweeping();
  o.startleActive = (startleState != STARTLE_OFF);
#if CLOCK
  o.clockOn = clockOn;
  o.clockSuppressed = clockOn && !timeSynced;
  o.clockSeconds = clockSeconds;
  o.clockRate = clockRate;
  o.clockSecOfDay = clockNow();
  for (uint8_t i = 0; i < 3; i++)
    o.clockColor[i] = clockRGB[i];
#else
  o.clockOn = o.clockSeconds = o.clockSuppressed = false;
  o.clockRate = 0;
  o.clockSecOfDay = 0;
  o.clockColor[0] = o.clockColor[1] = o.clockColor[2] = 0;
#endif

  o.cpuMhz = getCpuFrequencyMhz();
  o.cpuSetting = cpuSetting;
  o.settingsDirty = settingsDirty;
  o.fps = lastFps;
  o.freeHeap = ESP.getFreeHeap();
  o.uptimeSec = millis() / 1000UL;
}

uint8_t stateEyeCount(void) { return designCount(); }

const char *stateEyeName(uint8_t i) {
  return i < designCount() ? designAt(i).name : NULL;
}

// Queued rather than applied: see the note in state.h.  eyeDesign itself is
// updated only when the renderer picks the request up, so a caller reading it
// back immediately sees the old value for at most one frame -- which is why
// the API re-reads through stateGet after setting, and gets the pending value
// from there.
bool stateSetEyeIndex(long i) {
  LOCKED;
  if (i < 0 || i >= designCount())
    return false;
  pendingEyeDesign = (int16_t)i;
  return true;
}

bool stateSetEyeName(const char *name) {
  LOCKED;
  uint8_t i = eyeDesignByName(name);
  if (i >= designCount())
    return false;
  pendingEyeDesign = (int16_t)i;
  return true;
}

void stateNextEye(void) {
  LOCKED;
  // From whichever is the latest intention, so two presses in one frame move
  // two designs rather than fighting over one.
  uint8_t from = pendingEyeDesign >= 0 ? (uint8_t)pendingEyeDesign : eyeDesign;
  pendingEyeDesign = (int16_t)((from + 1) % designCount());
}

// Takes the loaded design out of the list, moving the renderer off it first.
//
// The one design change applied at once rather than queued for the next
// frame: the memory it names is about to be erased or unmapped, and a queued
// change would leave the renderer's pointers aimed at it until then.  Safe
// to apply here because every caller runs in the render task between frames
// -- the web server and the console are both served from frame(), before it
// draws.
static void dropLoadedDesign(void) {
  LOCKED;
  if (!loadedPresent)
    return;
  if (pendingEyeDesign == (int16_t)LOADED_DESIGN)
    pendingEyeDesign = 0;
  if (eyeDesign == LOADED_DESIGN)
    setEyeDesign(0);
  loadedPresent = false;
}

// Consulted by eyestore once an upload's header has passed its own checks.
// Refuses a name a built-in design already has -- `save` stores designs by
// name, so two with one name would be one too many -- or one the console's
// `eye` command would read as a keyword.  Otherwise clears the way for the
// erase.
static EyeLoadResult eyeUploadGate(const char *name) {
  static const char *const keywords[] = {"list", "next", "toggle", "unload"};
  for (const char *k : keywords)
    if (!strcmp(name, k))
      return EYE_LOAD_NAME_TAKEN;
  for (uint8_t i = 0; i < NUM_BUILTIN_DESIGNS; i++)
    if (!strcmp(name, eyeDesigns[i].name))
      return EYE_LOAD_NAME_TAKEN;
  dropLoadedDesign();
  // The next few seconds are spent erasing and writing flash from inside
  // the render loop, so the eyes stop; say why rather than freeze.
  showMessage("EYE", "LOADING", name, NULL);
  return EYE_LOAD_OK;
}

void stateEyeSlot(EyeSlotState &o) {
  LOCKED;
  o.available = eyeStoreAvailable();
  o.loaded = loadedPresent;
  o.name = loadedPresent ? loadedDesign.name : NULL;
  o.index = LOADED_DESIGN;
  o.capacity = eyeStoreCapacity();
}

void stateEyeLoadBegin(uint32_t contentLength) {
  eyeStoreWriteBegin(contentLength, eyeUploadGate);
}

void stateEyeLoadChunk(const uint8_t *data, size_t n) {
  eyeStoreWrite(data, n);
}

EyeLoadResult stateEyeLoadEnd(void) {
  EyeLoadResult r = eyeStoreWriteEnd();
  if (r == EYE_LOAD_OK) {
    LOCKED;
    adoptLoadedDesign();
    // Selected, because seeing it is why anybody uploads one.
    pendingEyeDesign = (int16_t)LOADED_DESIGN;
  }
  return r;
}

void stateEyeLoadAbort(void) { eyeStoreWriteAbort(); }

bool stateEyeUnload(void) {
  dropLoadedDesign();
  return eyeStoreErase();
}

bool stateSetGaze(long x, long y) {
  LOCKED;
  if (x < 0 || x > 1023 || y < 0 || y > 1023)
    return false;
  gazeCmdX = (int16_t)x; // in range, so the narrowing is exact
  gazeCmdY = (int16_t)y;
  gazeCmdActive = true;
  gazeCmdPending = true;
  return true;
}

void stateGazeAuto(void) {
  LOCKED;
  gazeCmdActive = false;
}

bool stateSetDilation(long pct) {
  LOCKED;
  if (pct < 0 || pct > 100)
    return false;
  startleCancel(); // an explicit width wins over a running effect
  setDilation((uint8_t)pct); // in range, so exact
  return true;
}

void stateDilationAuto(void) {
  LOCKED;
  startleCancel(); // else it restores a commanded width a moment later
  dilateCmdActive = false;
}

void stateSetPupil(bool on) {
  LOCKED;
  pupilOn = on;
  settingsDirty = true;
}

void stateSetSwap(bool sw) {
  LOCKED;
  if (sw == eyesSwapped)
    return;
  eyesSwapped = sw;
  swapPending = true; // applied between frames
  settingsDirty = true;
}

// Stored at once rather than by save, since it cannot be tried out live
// first -- see cpuSetting for why it waits for a restart.  Forget clears it
// with everything else.
bool stateSetCpu(long mhz) {
  if (!cpuValid(mhz))
    return false;
  LOCKED;
  if (!prefs.begin(PREFS_NAMESPACE, false))
    return false;
  const bool stored = prefs.putUChar(PREFS_KEY_CPU, (uint8_t)mhz) == 1;
  prefs.end();
  if (stored)
    cpuSetting = (uint8_t)mhz;
  return stored;
}

void stateRestart(void) {
  LOCKED;
  restartPending = true;
  restartAskedMs = millis();
}

bool stateSetFlip(uint8_t e, bool flipped) {
  LOCKED;
  if (e >= NUM_EYES)
    return false;
  // Resolved to the panel now, not at apply time: a swap queued in the same
  // gap moves the eye, and the setting must not follow it.
  bool &slot = panelFlip[flipSlot(eye[e].cs)];
  if (slot == flipped)
    return true;
  slot = flipped;
  flipPending = true; // applied between frames
  settingsDirty = true;
  return true;
}

bool stateDimSet(long percent) {
  LOCKED;
  if (percent < 0 || percent > 100)
    return false;
  dimmerSetPercent((uint8_t)percent); // the fade happens in the render loop
  settingsDirty = true;
  return true;
}

bool stateDimSetGamma(long gammaX10) {
  LOCKED;
  if (gammaX10 < DIM_GAMMA_MIN || gammaX10 > DIM_GAMMA_MAX)
    return false;
  dimmerSetGammaX10((uint8_t)gammaX10);
  settingsDirty = true;
  return true;
}

bool stateDimSetTrim(uint8_t e, long percent) {
  LOCKED;
  if (e >= NUM_EYES || percent < -50 || percent > 50)
    return false;
  // Resolved to the panel now, as stateSetFlip() does, and for its reason.
  dimmerSetTrim(flipSlot(eye[e].cs), (int8_t)percent);
  settingsDirty = true;
  return true;
}

void stateDimSweep(bool on) {
  LOCKED;
  dimmerSetSweep(on);
}

void stateBlink(void) {
  LOCKED;
#if AUTOBLINK
  timeToNextBlink = 0; // due on the next frame
#endif
}

void stateStartle(void) {
  LOCKED;
  startleBegin();
}

void stateSplash(void) {
  LOCKED;
#if STARTUP_SPLASH
  splashBegin(); // returns at once; the render loop counts it down
#endif
}

void stateClockSetOn(bool on) {
  LOCKED;
#if CLOCK
  clockOn = on;
  settingsDirty = true;
#endif
}

void stateClockSetSeconds(bool on) {
  LOCKED;
#if CLOCK
  clockSeconds = on;
  settingsDirty = true;
#endif
}

bool stateClockSetRate(long rate) {
  LOCKED;
#if CLOCK
  if (rate < 1 || rate > 3600)
    return false;
  clockSet(clockNow()); // rebase so the change is not retroactive
  clockRate = (uint16_t)rate;
  settingsDirty = true;
  return true;
#else
  (void)rate;
  return false;
#endif
}

bool stateClockSetTime(long h, long m, long sec) {
  LOCKED;
#if CLOCK
  if (h < 0 || h > 23 || m < 0 || m > 59 || sec < 0 || sec > 59)
    return false;
  // Deliberately not marked dirty: the time is not persisted in NVS.  With an
  // RTC fitted it goes somewhere better instead.
  clockSet((uint32_t)h * 3600UL + (uint32_t)m * 60UL + sec);

  // Keep today's date if a source has already supplied one; otherwise start
  // from a fixed date: something has to carry the time, and a wrong date is
  // harmless here -- nothing displays one.
  struct tm t;
  if (!timeLocal(t)) {
    memset(&t, 0, sizeof(t));
    t.tm_year = 2026 - 1900;
    t.tm_mday = 1;
  }
  t.tm_hour = h;
  t.tm_min = m;
  t.tm_sec = sec;
  t.tm_isdst = -1; // let the zone's own rules decide
  // mktime reads the fields as local time and hands back a UTC epoch, which
  // is what both the system clock and the chip want.
  time_t utc = mktime(&t);
  if (utc > 0) {
    timeAccept(utc, TIME_MANUAL);
#if RTC
    if (rtcPresent())
      rtcWrite(utc);
#endif
  }
  return true;
#else
  (void)h; (void)m; (void)sec;
  return false;
#endif
}

bool stateClockSetColor(long which, uint32_t rgb) {
  LOCKED;
#if CLOCK
  if (which < -1 || which >= 3 || rgb > 0xFFFFFF)
    return false;
  if (which < 0)
    for (uint8_t i = 0; i < 3; i++)
      clockSetColor(i, rgb);
  else
    clockSetColor((uint8_t)which, rgb);
  settingsDirty = true;
  return true;
#else
  (void)which; (void)rgb;
  return false;
#endif
}

bool stateSleepSet(const SleepChange &c) {
  LOCKED;
#if SLEEP
  if ((c.setWindow && (c.start < 0 || c.start > 1439 || c.stop < 0 ||
                       c.stop > 1439)) ||
      (c.setLevel && (c.level < 0 || c.level > 100)))
    return false;
  sleepCancelWake();
  if (c.setWindow)
    sleepSetWindow((uint16_t)c.start, (uint16_t)c.stop);
  if (c.setLevel)
    sleepSetLevel((uint8_t)c.level);
  if (c.setEnabled)
    sleepSetEnabled(c.enabled);
  settingsDirty = true;
  return true;
#else
  (void)c;
  return false;
#endif
}

bool stateTzSet(const char *nameOrPosix) {
  LOCKED;
  if (!timeSetTz(nameOrPosix))
    return false;
  settingsDirty = true;
#if NETWORK
  netRequestTimeRestart();
#endif
  return true;
}

void stateNtpSetEnabled(bool on) {
  LOCKED;
#if NETWORK
  netNtpSetEnabled(on);
  settingsDirty = true;
#else
  (void)on;
#endif
}

void stateSave(void) {
  LOCKED;
  saveSettings();
}

void stateForget(void) {
  LOCKED;
  forgetSettings();
}

#endif // CONTROLLABLE

// SERIAL CONSOLE ------------------------------------------------------------
// Everything from here to the end of the block is the console itself: the
// help text, the line parser, and the BOOT button.  It sits on the operations
// layer above and knows nothing the API does not.

#if COMMANDS

// Kept in flash with F() -- the string is longer than it looks.
static void cmdHelp(Print &out) {
  out.print(F("\ncommands:\n"
                 "  eye                       list the designs, and the slot\n"
                 "  eye <name>|<index>|next   select an eye design\n"
                 "  eye unload                empty the eye slot\n"
                 "  look <x> <y>              aim gaze, 0-1023 each "
                 "(512 512 = centre)\n"
                 "  look auto                 return to autonomous motion\n"
                 "  dilate <0-100>            pupil width, 100 = fully "
                 "dilated\n"
                 "  dilate auto               return to autonomous dilation\n"
                 "  startle                   constrict, then snap wide "
                 "with a blink\n"
                 "  clock [on|off]            analogue clock in the iris\n"
                 "  clock set HH:MM[:SS]      set the time\n"
                 "  clock rate <1-3600>       run it faster, for testing\n"
                 "  clock secs [on|off]       show the second hand\n"
                 "  clock color [hour|min|sec] RRGGBB\n"
#if SLEEP
                 "  sleep [on|off]            dark panels overnight\n"
                 "  sleep HH:MM HH:MM         when to sleep, and when to "
                 "wake\n"
                 "  sleep level <0-100>       0 turns the panels off\n"
#endif
                 "  net [quiet]               address info, on screen too\n"
                 "  net off                   dismiss the address cards\n"
                 "  wifi                      the network, and how to change "
                 "it\n"
                 "  version                   firmware version and commit\n"
                 "  warnings                  problems found at boot\n"
#if NETWORK
                 "  ntp [on|off|sync]         use a time server, or stop\n"
#endif
#if RTC
                 "  rtc [sync]                battery-backed clock\n"
#endif
                 "  tz [POSIX string]         timezone, e.g. CST6CDT,M3.2.0/2\n"
                 "  pupil [on|off]            pupil, or a full iris disc\n"
                 "  swap [on|off]             swap which panel is which "
                 "eye\n"
                 "  dim [0-100]               brightness; 0 is off\n"
                 "  dim gamma <1.0-4.0>       the brightness curve\n"
                 "  dim trim left|right <n>   even out two panels, -50..50\n"
                 "  dim sweep [off]           slow full-range sweep\n"
                 "  cpu [160|240]             CPU speed, from the next "
                 "restart\n"
                 "  restart                   restart the board\n"
                 "  flip left|right [on|off]  turn a panel mounted upside "
                 "down\n"
                 "  save                      remember settings across "
                 "reboots\n"
                 "  forget                    clear saved settings\n"
                 "  blink                     blink both eyes now\n"
                 "  splash                    re-show the panel name cards\n"
                 "  status                    report current state\n"
                 "  help                      this list\n"));
}

// The brightness, and what feeds it.  "shown" differs from the setting
// during a fade, a sweep, or while sleep mode has the eyes dimmed.
static void cmdDim(Print &out) {
  DeviceState s;
  stateGet(s);
  out.printf("dim=%u%% shown=%u%% gamma=%u.%u trim left=%+d right=%+d%s\n",
             (unsigned)s.dimPercent, (unsigned)s.dimShown,
             (unsigned)(s.dimGammaX10 / 10), (unsigned)(s.dimGammaX10 % 10),
             s.dimTrim[0], s.dimTrim[1], s.dimSweeping ? " (sweeping)" : "");
}

// What the board found wrong at boot -- see health.h for what that covers.
// The control page shows the same list.
static void cmdWarnings(Print &out) {
  if (!healthCount()) {
    out.println(F("no warnings"));
    return;
  }
  for (uint8_t i = 0; i < healthCount(); i++)
    out.printf("  %s\n", healthLine(i));
  if (healthDropped())
    out.printf("  ...and %u more\n", (unsigned)healthDropped());
}

// One line of everything worth knowing, plus an (unsaved) marker when the
// live settings differ from the stored ones.
static void cmdStatus(Print &out) {
  out.printf("eye=%u/%u %s gaze=%s", (unsigned)eyeDesign,
                (unsigned)designCount(), designAt(eyeDesign).name,
                gazeCmdActive ? "commanded" : "auto");
  if (gazeCmdActive)
    out.printf("(%d,%d)", gazeCmdX, gazeCmdY);
  out.printf(" dilate=%s", dilateCmdActive ? "" : "auto");
  if (dilateCmdActive)
    out.printf("%u%%", (unsigned)dilateCmdPct);
  if (startleState != STARTLE_OFF)
    out.print(startleState == STARTLE_WINDUP ? " startle=windup"
                                                : " startle=hold");
  out.printf(" pupil=%s", pupilOn ? "on" : "off");
  out.printf(" swap=%s flip=%s%s%s", eyesSwapped ? "on" : "off",
                panelFlip[flipSlot(eye[0].cs)] ? "L" : "-",
                NUM_EYES > 1 && panelFlip[flipSlot(eye[NUM_EYES - 1].cs)]
                    ? "R" : "-",
                settingsDirty ? " (unsaved)" : "");
  out.printf(" dim=%u%% cpu=%uMHz", (unsigned)dimmerPercent(),
             (unsigned)getCpuFrequencyMhz());
  if (cpuSetting != getCpuFrequencyMhz())
    out.printf("(%u at restart)", (unsigned)cpuSetting);
  out.printf(" panel=%s heap=%u up=%us",
                USE_SSD1327 ? "ssd1327" : "ssd1351",
                (unsigned)ESP.getFreeHeap(), (unsigned)(millis() / 1000));
#if DEBUG
  out.printf(" fps=%u", lastFps);
#endif
  out.printf(" idle0gap=%ums", (unsigned)(idle0MaxGap * portTICK_PERIOD_MS));
#if OVERLAP_SEND
  out.printf(" rests=%u missed=%u", (unsigned)senderRests,
             (unsigned)senderRestsMissed);
#endif
  out.println();
}

// Splits one line into a command and its arguments and dispatches it.
// strtok chews up the buffer, which is fine -- the caller owns it and
// discards it afterwards.  The command word is lowercased; arguments are
// only lowercased where case should not matter, such as eye names.
void handleCommand(char *line, Print &out) {
#if SLEEP
  // A typed command is a person, so the eyes come up even mid-window.  This
  // covers /cmd as well, which lands here.
  sleepNudge();
#endif
  char *cmd = strtok(line, " \t");
  if (!cmd)
    return;
  for (char *c = cmd; *c; c++)
    *c = (char)tolower((unsigned char)*c);

  if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
    cmdHelp(out);
  } else if (!strcmp(cmd, "version")) {
    out.printf("frank %s (%s), built %s\n", FIRMWARE_VERSION,
               FIRMWARE_COMMIT, __DATE__ " " __TIME__);
    out.println(F(PROJECT_URL));
  } else if (!strcmp(cmd, "status")) {
    cmdStatus(out);
  } else if (!strcmp(cmd, "warnings")) {
    cmdWarnings(out);
  } else if (!strcmp(cmd, "eye")) {
    char *arg = strtok(NULL, " \t");
    if (arg)
      for (char *c = arg; *c; c++)
        *c = (char)tolower((unsigned char)*c);
    if (!arg || !strcmp(arg, "list")) { // bare "eye" reports what is available
      listEyeDesigns(out);
      return;
    }
    if (!strcmp(arg, "unload")) {
      if (!stateEyeUnload()) {
        out.println(F("err: no eye slot on this board, or erasing it failed"));
        return;
      }
      out.printf("ok slot empty; eye=%u %s\n", (unsigned)eyeDesign,
                 designAt(eyeDesign).name);
      return;
    }
    if (!strcmp(arg, "next") || !strcmp(arg, "toggle")) {
      stateNextEye();
    } else if (arg[0] >= '0' && arg[0] <= '9') { // by index
      long idx;
      if (!parseLong(arg, 0, 255, idx) || !stateSetEyeIndex(idx)) {
        out.printf("err: no design %s -- there are %u\n", arg,
                   (unsigned)stateEyeCount());
        return;
      }
    } else if (!stateSetEyeName(arg)) { // by name
        out.printf("err: no design '%s'. built in:\n", arg);
      listEyeDesigns(out);
      return;
    }
    // The requested design, from stateGet(): eyeDesign itself only changes
    // when the renderer picks the request up, on the next frame, so reading
    // it here reported the design being replaced.
    DeviceState s;
    stateGet(s);
    out.printf("ok eye=%u %s\n", (unsigned)s.eyeIndex, s.eyeName);
  } else if (!strcmp(cmd, "look")) {
    char *a1 = strtok(NULL, " \t");
    if (!a1) {
      out.println(F("usage: look <0-1023> <0-1023> | look auto"));
      return;
    }
    if (!strcmp(a1, "auto")) {
      stateGazeAuto();
      out.println(F("ok gaze=auto"));
      return;
    }
    char *a2 = strtok(NULL, " \t");
    if (!a2) {
      out.println(F("usage: look <0-1023> <0-1023> | look auto"));
      return;
    }
    long x, y;
    if (!parseLong(a1, 0, 1023, x) || !parseLong(a2, 0, 1023, y) ||
        !stateSetGaze(x, y)) {
      out.println(F("err: both values must be 0-1023"));
      return;
    }
    out.printf("ok gaze=(%ld,%ld)\n", x, y);
#if SLEEP
  } else if (!strcmp(cmd, "sleep")) {
    char *a = strtok(NULL, " \t");
    if (!a) {
      uint16_t mins;
      bool toAsleep;
      out.printf("sleep %s %02u:%02u-%02u:%02u level=%u (%s)",
                 sleepEnabled() ? "on" : "off", sleepStart() / 60,
                 sleepStart() % 60, sleepStop() / 60, sleepStop() % 60,
                 (unsigned)sleepLevel(), sleepReason());
      if (sleepNextChange(mins, toAsleep))
        out.printf(", %s in %uh%02um", toAsleep ? "sleeps" : "wakes",
                   mins / 60, mins % 60);
      out.println();
    } else if (!strcmp(a, "on") || !strcmp(a, "off")) {
      SleepChange c;
      c.setEnabled = true;
      c.enabled = !strcmp(a, "on");
      stateSleepSet(c);
      out.printf("ok sleep=%s\n", sleepEnabled() ? "on" : "off");
    } else if (!strcmp(a, "level")) {
      char *v = strtok(NULL, " \t");
      long pct;
      if (!parseLong(v, 0, 100, pct)) {
        out.println(F("usage: sleep level <0-100>   (0 = panels off)"));
      } else {
        SleepChange c;
        c.setLevel = true;
        c.level = pct;
        stateSleepSet(c);
        out.printf("ok sleep level=%u\n", (unsigned)sleepLevel());
      }
    } else {
      // "sleep 22:00 07:00"
      char *b = strtok(NULL, " \t");
      uint8_t h1, m1, h2, m2, unused;
      if (!parseTimeOfDay(a, false, h1, m1, unused) ||
          !parseTimeOfDay(b, false, h2, m2, unused)) {
        out.println(F("usage: sleep HH:MM HH:MM   (start, then stop)"));
      } else {
        SleepChange c;
        c.setWindow = true;
        c.start = h1 * 60 + m1;
        c.stop = h2 * 60 + m2;
        stateSleepSet(c);
        out.printf("ok sleep %02u:%02u-%02u:%02u\n", h1, m1, h2, m2);
      }
    }
#endif
#if CLOCK
  } else if (!strcmp(cmd, "clock")) {
    char *arg = strtok(NULL, " \t");
    if (!arg) {
      uint32_t t = clockNow();
      out.printf("clock %s %02u:%02u:%02u rate=%ux seconds=%s\n",
                    clockOn && !timeSynced ? "on, hidden -- the time is unknown"
                    : clockOn                 ? "on"
                                              : "off",
                    (unsigned)(t / 3600),
                    (unsigned)((t / 60) % 60), (unsigned)(t % 60),
                    (unsigned)clockRate, clockSeconds ? "on" : "off");
      out.printf("  colours hour=%06lX min=%06lX sec=%06lX\n",
                    (unsigned long)clockRGB[0], (unsigned long)clockRGB[1],
                    (unsigned long)clockRGB[2]);
      return;
    }
    for (char *c = arg; *c; c++)
      *c = (char)tolower((unsigned char)*c);

    if (!strcmp(arg, "on") || !strcmp(arg, "off")) {
      stateClockSetOn(!strcmp(arg, "on"));
      out.printf("ok clock=%s\n", clockOn ? "on" : "off");
    } else if (!strcmp(arg, "set")) {
      char *v = strtok(NULL, " \t");
      uint8_t h, m, sec;
      if (!parseTimeOfDay(v, true, h, m, sec)) {
        out.println(F("usage: clock set HH:MM[:SS]"));
        return;
      }
      // Through the operations layer rather than clockSet() directly: that
      // is what carries the write-through to the RTC, and reaching around
      // it is exactly how the console and the API drift apart.
      if (!stateClockSetTime(h, m, sec)) {
        out.println(F("err: out of range"));
        return;
      }
      out.printf("ok clock set %02u:%02u:%02u\n", h, m, sec);
    } else if (!strcmp(arg, "rate")) {
      char *v = strtok(NULL, " \t");
      long r;
      if (!parseLong(v, 1, 3600, r) || !stateClockSetRate(r)) {
        out.println(F("usage: clock rate <1-3600>"));
        return;
      }
      out.printf("ok clock rate=%ldx\n", r);
    } else if (!strcmp(arg, "color") || !strcmp(arg, "colour")) {
      char *a = strtok(NULL, " \t");
      char *b = strtok(NULL, " \t");
      if (!a) {
        out.println(F("usage: clock color [hour|min|sec] RRGGBB"));
        return;
      }
      int8_t which = -1; // -1 means all three
      char *hex = a;
      if (b) {
        for (char *c = a; *c; c++)
          *c = (char)tolower((unsigned char)*c);
        if (!strcmp(a, "hour"))
          which = 0;
        else if (!strcmp(a, "min"))
          which = 1;
        else if (!strcmp(a, "sec"))
          which = 2;
        else {
          out.println(F("usage: clock color [hour|min|sec] RRGGBB"));
          return;
        }
        hex = b;
      }
      uint32_t v;
      if (!parseHexColor(hex, v) || !stateClockSetColor(which, v)) {
        out.println(F("err: colour must be 6 hex digits, e.g. FF8800"));
        return;
      }
      out.printf("ok clock color %s=%06lX\n",
                 which < 0    ? "all"
                 : which == 0 ? "hour"
                 : which == 1 ? "min"
                              : "sec",
                 (unsigned long)v);
    } else if (!strcmp(arg, "secs")) {
      char *v = strtok(NULL, " \t");
      stateClockSetSeconds(!(v && !strcmp(v, "off")));
      out.printf("ok seconds=%s\n", clockSeconds ? "on" : "off");
    } else {
      out.println(
          F("usage: clock [on|off|set HH:MM[:SS]|rate N|secs on|off|"
            "color [hour|min|sec] RRGGBB]"));
    }
#endif
  } else if (!strcmp(cmd, "tz")) {
    // Everything after the command word, so the POSIX string keeps its commas
    // and slashes intact.
    char *rest = strtok(NULL, "");
    while (rest && *rest == ' ')
      rest++;
    if (!rest || !*rest) {
      out.printf("tz %s (time from %s)\n", tzString, timeSourceName());
      const char *region = NULL;
      for (uint8_t i = 0; i < numTzChoices; i++) {
        if (!region || strcmp(region, tzChoices[i].region)) {
          region = tzChoices[i].region;
          out.printf("\n  %s:\n   ", region);
        }
        out.printf(" %s", tzChoices[i].name);
      }
      out.println();
      out.println(F("  or any POSIX string, e.g. PST8PDT,M3.2.0/2,M11.1.0/2"));
      return;
    }
    if (!stateTzSet(rest)) {
      out.println(F("err: not a known zone name or a POSIX timezone string"));
      return;
    }
    out.printf("ok tz=%s\n", tzString);

#if NETWORK
  } else if (!strcmp(cmd, "ntp")) {
    char *arg = strtok(NULL, " \t");
    if (arg)
      for (char *c = arg; *c; c++)
        *c = (char)tolower((unsigned char)*c);
    if (arg && !strcmp(arg, "sync")) {
      if (!netNtpEnabled())
        out.println(F("err: ntp is off"));
      else if (!netNtpSyncNow())
        out.println(F("err: no link yet, so there is nothing to ask"));
      else
        out.println(F("ok asking now"));
      return;
    }
    if (arg && (!strcmp(arg, "on") || !strcmp(arg, "off"))) {
      stateNtpSetEnabled(!strcmp(arg, "on"));
    } else if (arg) {
      out.println(F("usage: ntp [on|off|sync]"));
      return;
    }
    NtpStatus n;
    netNtpStatus(n);
    out.printf("ntp %s", n.enabled ? "on" : "off");
    if (n.enabled) {
      if (n.synced)
        out.printf(", last answer %us ago, asking every %us",
                   (unsigned)n.lastSyncSec, (unsigned)n.intervalSec);
      else
        out.printf(", %s", n.linkUp ? "waiting for a reply" : "no link");
      out.printf(" (%s)", n.server);
    }
    out.println();
#endif

#if RTC
  } else if (!strcmp(cmd, "rtc")) {
    char *arg = strtok(NULL, " \t");
    if (!arg) {
      rtcReport(out);
    } else if (!strcmp(arg, "sync")) {
      // Useful after `clock set` on a board with no network: it puts what is
      // on the face into the chip, where it survives the power going off.
      if (!rtcPresent())
        out.println(F("err: no RTC found"));
      else if (!rtcWriteNow())
        out.println(F("err: the clock has no real time to store yet"));
      else
        out.println(F("ok rtc written from the current time"));
    } else {
      out.println(F("usage: rtc [sync]"));
    }
#endif

#if NETWORK
  } else if (!strcmp(cmd, "wifi")) {
    char *arg = strtok(NULL, " \t");
    if (!arg) {
      out.printf("wifi ssid=%s state=%s", WiFi.SSID().c_str(),
                 WiFi.status() == WL_CONNECTED ? "up" : "down");
      if (WiFi.status() == WL_CONNECTED)
        out.printf(" rssi=%d", WiFi.RSSI());
      out.println();
      out.println(F("  wifi join <ssid> [password]   store and reboot into it"
                    "\n  wifi forget                   clear the stored network"
                    "\n  wifi portal                   reboot into the setup "
                    "portal"));
      return;
    }
    if (!strcmp(arg, "forget")) {
      netRequestForget();
      out.println(F("ok forgetting the stored network; rebooting"));
    } else if (!strcmp(arg, "portal")) {
      netRequestPortal();
      out.printf("ok rebooting into the portal; join '%s'\n", WIFI_AP_NAME);
    } else if (!strcmp(arg, "join")) {
      // The SSID may contain spaces, the password may not -- so the password
      // is taken as the last word and the SSID as everything before it.
      char *rest = strtok(NULL, "");
      while (rest && *rest == ' ')
        rest++;
      if (!rest || !*rest) {
        out.println(F("err: usage: wifi join <ssid> [password]"));
        return;
      }
      char *pass = strrchr(rest, ' ');
      if (pass) {
        *pass++ = '\0';
        while (*pass == ' ')
          pass++;
      }
      if (!netRequestJoin(rest, pass ? pass : "")) {
        out.println(F("err: ssid must be 1-32 characters, password under 64"));
        return;
      }
      out.printf("ok storing '%s'; rebooting\n", rest);
    } else {
      out.println(F("err: wifi takes join, forget or portal"));
    }
  } else if (!strcmp(cmd, "net")) {
    char *arg = strtok(NULL, " \t");
    if (arg && !strcmp(arg, "off")) { // take the panels back early
      netHide();
      out.println(F("ok address cards dismissed"));
      return;
    }
    netReport(out);
    if (!arg || strcmp(arg, "quiet")) {
      netShow();
      out.printf("ok showing address cards for %us -- `net off` to dismiss\n",
                 (unsigned)(NET_SHOW_MS / 1000));
    }
#endif
  } else if (!strcmp(cmd, "pupil")) {
    char *arg = strtok(NULL, " \t");
    if (arg)
      for (char *c = arg; *c; c++)
        *c = (char)tolower((unsigned char)*c);
    if (!arg)
      stateSetPupil(!pupilOn);
    else if (!strcmp(arg, "on"))
      stateSetPupil(true);
    else if (!strcmp(arg, "off"))
      stateSetPupil(false);
    else {
      out.println(F("usage: pupil [on|off]"));
      return;
    }
    out.printf("ok pupil=%s%s\n", pupilOn ? "on" : "off",
                  pupilOn ? "" : " (full iris disc; dilate has no effect)");
  } else if (!strcmp(cmd, "cpu")) {
    char *arg = strtok(NULL, " \t");
    long mhz;
    if (arg && (!parseLong(arg, 0, 1000, mhz) || !cpuValid(mhz))) {
      out.println(F("usage: cpu [160|240]"));
      return;
    }
    if (arg && !stateSetCpu(mhz)) {
      out.println(F("error: the setting could not be stored"));
      return;
    }
    out.printf("%scpu=%u MHz", arg ? "ok " : "", (unsigned)getCpuFrequencyMhz());
    if (cpuSetting != getCpuFrequencyMhz())
      out.printf(", %u MHz after a restart", (unsigned)cpuSetting);
    out.println();
  } else if (!strcmp(cmd, "restart")) {
    out.println(settingsDirty ? F("ok restarting; unsaved changes are lost")
                              : F("ok restarting"));
    stateRestart();
  } else if (!strcmp(cmd, "swap")) {
    char *arg = strtok(NULL, " \t");
    bool want = !eyesSwapped;
    if (arg) {
      if (!strcmp(arg, "on"))
        want = true;
      else if (!strcmp(arg, "off"))
        want = false;
      else {
        out.println(F("usage: swap [on|off]"));
        return;
      }
    }
    stateSetSwap(want);
    out.printf("ok swap=%s\n", eyesSwapped ? "on" : "off");
  } else if (!strcmp(cmd, "dim")) {
    char *a = strtok(NULL, " \t");
    long v;
    bool ok = true;
    if (!a) {
      // bare `dim` reports
    } else if (!strcmp(a, "gamma")) {
      ok = parseTenths(strtok(NULL, " \t"), DIM_GAMMA_MIN, DIM_GAMMA_MAX, v) &&
           stateDimSetGamma(v);
    } else if (!strcmp(a, "trim")) {
      // Your left and right, facing the head, as for `flip`.
      char *side = strtok(NULL, " \t");
      char *n = strtok(NULL, " \t");
      uint8_t e = !side ? 2 : !strcmp(side, "left") ? 0 : !strcmp(side, "right") ? 1 : 2;
      ok = e < 2 && parseLong(n, -50, 50, v) && stateDimSetTrim(e, v);
    } else if (!strcmp(a, "sweep")) {
      char *o = strtok(NULL, " \t");
      stateDimSweep(!(o && !strcmp(o, "off")));
    } else {
      ok = parseLong(a, 0, 100, v) && stateDimSet(v);
    }
    if (!ok) {
      out.println(F("usage: dim [0-100] | dim gamma <1.0-4.0> | "
                    "dim trim left|right <-50..50> | dim sweep [off]"));
      return;
    }
    cmdDim(out);
  } else if (!strcmp(cmd, "flip")) {
    // Left and right are YOURS, facing the head -- the "YOUR LEFT" line of
    // the splash -- because that is the side you can see is upside down.
    char *side = strtok(NULL, " \t");
    char *arg = strtok(NULL, " \t");
    uint8_t e;
    if (side && !strcmp(side, "left"))
      e = 0;
    else if (side && !strcmp(side, "right"))
      e = 1;
    else {
      out.println(F("usage: flip left|right [on|off]"));
      return;
    }
    DeviceState s;
    stateGet(s);
    bool want = !s.flipped[e];
    if (arg) {
      if (!strcmp(arg, "on"))
        want = true;
      else if (!strcmp(arg, "off"))
        want = false;
      else {
        out.println(F("usage: flip left|right [on|off]"));
        return;
      }
    }
    if (!stateSetFlip(e, want)) {
      out.println(F("no such panel in this build"));
      return;
    }
    out.printf("ok flip %s=%s\n", side, want ? "on" : "off");
  } else if (!strcmp(cmd, "save")) {
    stateSave();
    out.printf("ok saved eye=%s swap=%s\n",
                  designAt(eyeDesign).name, eyesSwapped ? "on" : "off");
  } else if (!strcmp(cmd, "forget")) {
    stateForget();
    out.println(F("ok settings cleared; build defaults apply at next boot"));
  } else if (!strcmp(cmd, "startle")) {
    stateStartle();
    out.println(F("ok startle"));
  } else if (!strcmp(cmd, "dilate")) {
    char *arg = strtok(NULL, " \t");
    if (!arg) {
      out.println(F("usage: dilate <0-100> | dilate auto"));
      return;
    }
    if (!strcmp(arg, "auto")) {
      stateDilationAuto();
      out.println(F("ok dilate=auto"));
      return;
    }
    long pct;
    if (!parseLong(arg, 0, 100, pct) || !stateSetDilation(pct)) {
      out.println(F("err: dilation must be 0-100"));
      return;
    }
    out.printf("ok dilate=%ld%%\n", pct);
#if AUTOBLINK
  } else if (!strcmp(cmd, "blink")) {
    stateBlink();
    out.println(F("ok blink"));
#endif
#if STARTUP_SPLASH
  } else if (!strcmp(cmd, "splash")) {
    stateSplash();
    out.println(F("ok splash"));
#endif
  } else {
    out.printf("unknown command '%s' -- try 'help'\n", cmd);
  }
}

// Non-blocking: called once per rendered frame (see loop() for why).
static void pollCommands(void) {
  static char line[64];
  static uint8_t len = 0;

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r')
      continue;
    if (c == '\n') {
      line[len] = '\0';
      if (len)
        handleCommand(line, Serial);
      len = 0;
    } else if (len < sizeof(line) - 1) {
      line[len++] = c;
    } else {
      len = 0; // overlong line, discard rather than truncate
    }
  }
}

// Debounced edge detect on the BOOT button, acting on press rather than
// release so it feels immediate.  40 ms is enough for these switches.
static void pollBootButton(void) {
  static bool wasDown = false;
  static uint32_t lastEdge = 0;
  bool isDown = (digitalRead(BOOT_BUTTON_PIN) == LOW);
  uint32_t now = millis();

  if (isDown != wasDown && (now - lastEdge) > 40) { // debounce
    lastEdge = now;
    wasDown = isDown;
    if (isDown) { // act on press, not release
#if SLEEP
      sleepNudge();
#endif
      stateNextEye();
      DeviceState s; // the requested design; see the `eye` command
      stateGet(s);
      Serial.printf("ok eye=%u %s (button)\n", (unsigned)s.eyeIndex, s.eyeName);
    }
  }

#if FACTORY_RESET_MS
  // Held down long enough, this wipes every stored setting -- see
  // FACTORY_RESET_MS.  It is the only way back into a board whose password
  // has been forgotten, which is why it exists and why it is deliberately
  // awkward.
  //
  // Nothing held: forget any countdown in progress, so that the next press
  // starts from the top.  Letting go also needs no repainting -- drawEye()
  // covers the whole panel at the end of this same frame.
  static uint32_t lastDrawn = 0;
  if (!wasDown) {
    lastDrawn = 0;
    return;
  }

  uint32_t held = now - lastEdge;
  if (held >= FACTORY_RESET_MS) {
    Serial.printf("ok factory reset (button held %us)\n",
                  (unsigned)(FACTORY_RESET_MS / 1000));
    showMessage("FACTORY", "RESET", NULL, NULL);
    stateForget();
#if NETWORK
    credForget();
#endif
    delay(1500); // long enough to read, and nothing else matters now
    ESP.restart();
  }

  // Warn before wiping, not after.  Counting down from halfway leaves time to
  // let go, and says plainly what letting go avoids.
  if (held >= FACTORY_RESET_MS / 2) {
    uint32_t left = (FACTORY_RESET_MS - held + 999) / 1000;
    if (left != lastDrawn) {
      lastDrawn = left;
      char buf[8];
      snprintf(buf, sizeof(buf), "%u", (unsigned)left);
      showMessage("ERASE ALL", buf, "let go to", "cancel");
    }
  }
#endif
}

#endif // COMMANDS

void frame(            // Process motion for a single frame of left or right eye
    uint16_t iScale) { // Iris scale, IRIS_MIN..IRIS_MAX
#if DEBUG || CONTROLLABLE
  static uint32_t frames = 0; // frames drawn since the last rate report
#endif
  static uint8_t eyeIndex = 0; // eye[] array counter
  int16_t eyeX, eyeY;
  uint32_t t; // Time at start of function
  // The only survivor of the original UART command protocol: while set, the
  // motion code below holds the commanded gaze instead of drifting.
  static uint16_t serEyeCtrl = 0;

  if (++eyeIndex >= NUM_EYES)
    eyeIndex = 0; // Cycle through eyes, 1 per call

#if COMMANDS
  pollCommands();
  pollBootButton();
#endif

#if NETWORK
  netShowPoll(); // address cards, if something asked for them
  netPollLink(); // a network that turned up after boot
  netPollTime(); // cheap no-op once the first sync has landed
  webPoll();
  netPollPending(); // after webPoll, so a reply is sent before any reboot
#endif

#if CLOCK
  if (clockOn && timeSynced)
    clockUpdate();
#endif

#if CONTROLLABLE
  statePollPending(); // eye design and anything else queued by another task

  if (swapPending) { // between frames, never mid-transaction
    swapPending = false;
    applySwap();
  }
  if (flipPending) { // after the swap: see applyFlips()
    flipPending = false;
    applyFlips();
  }
  if (restartPending && millis() - restartAskedMs >= RESTART_DELAY_MS) {
    showMessage("RESTART", NULL, NULL, NULL);
    ESP.restart();
  }

  pollStartle();

  // Ease toward the commanded width rather than snapping.  While released,
  // track the autonomous value so handing control back is seamless.
  if (dilateCmdActive) {
    int32_t diff = (int32_t)dilateCmdValue - dilateCurrent;
    if (diff) {
      // Integer division alone stalls once the gap is smaller than the
      // divisor, leaving the pupil short of the commanded width, so always
      // move at least one step.
      int32_t step = diff / (int32_t)dilateEaseDiv;
      if (!step)
        step = (diff > 0) ? 1 : -1;
      dilateCurrent += step;
    }
    iScale = (uint16_t)dilateCurrent;
  } else {
    dilateCurrent = (int32_t)iScale;
  }
#endif


#if DEBUG || CONTROLLABLE
  // The frame rate, once a second.
  //
  // Measured whenever anything consumes it, rather than only when DEBUG is
  // on: the console's `status` and the API's system.fps both read it, and
  // tying the measurement to a diagnostics switch meant a DEBUG=0 build
  // reported a frame rate of zero for ever.
  {
    static uint32_t lastReport = 0;
    uint32_t now = millis();
    uint32_t elapsed = now - lastReport;
    if (elapsed >= 1000) {
      // Per second, not per interval.  The interval is at least a second but
      // has no upper bound -- anything that blocks the render loop stretches
      // it -- so reporting the raw count described a rate that had never
      // happened.
      uint16_t fps = (uint16_t)((frames * 1000UL) / elapsed);
#if DEBUG
      // Heartbeat: proves the render loop is alive even with no displays
      // wired, and the LED proves it without a serial cable.
      // Per frame drawn, in tenths of a millisecond: drawing, sending, and
      // the rest of the interval.  frames can be 0 (cards, sleep, off).
      // "wait" is the render loop waiting for the previous frame to be
      // sent; with OVERLAP_SEND the send itself happens on the other core
      // at the same time, so draw + wait + other is the frame, not the send.
      portENTER_CRITICAL(&perfMux);
      const uint32_t sendUs = perfSendUs;
      portEXIT_CRITICAL(&perfMux);
      const uint32_t n = frames ? frames : 1;
      const uint32_t drawT = perfDrawUs / n / 100, waitT = perfWaitUs / n / 100;
      const uint32_t sendT = sendUs / n / 100;
      // In 64 bits: the interval has no upper bound, and in microseconds 32
      // bits wrap at 71 minutes.  The perf counters stay 32-bit because they
      // only grow while frames are drawn and sent, which a stall stops.
      const uint64_t elapsedUs = (uint64_t)elapsed * 1000;
      const uint32_t restT = frames && elapsedUs > perfEyeUs
                                 ? (uint32_t)((elapsedUs - perfEyeUs) / n / 100)
                                 : 0;
      DEBUG_PRINTF("[creeper-eyes] fps=%u draw=%u.%ums wait=%u.%ums "
                   "send=%u.%ums other=%u.%ums heap=%u\n",
                   (unsigned)fps, (unsigned)(drawT / 10), (unsigned)(drawT % 10),
                   (unsigned)(waitT / 10), (unsigned)(waitT % 10),
                   (unsigned)(sendT / 10), (unsigned)(sendT % 10),
                   (unsigned)(restT / 10), (unsigned)(restT % 10),
                   (unsigned)ESP.getFreeHeap());
      digitalWrite(DEBUG_LED_PIN, !digitalRead(DEBUG_LED_PIN));
#endif
#if CONTROLLABLE
      lastFps = fps;
#endif
      frames = 0;
      perfDrawUs = perfWaitUs = perfEyeUs = 0;
      portENTER_CRITICAL(&perfMux);
      perfSendUs = 0;
      portEXIT_CRITICAL(&perfMux);
      lastReport = now;
    }
  }
#endif

  // X/Y movement

  t = micros();

  // Autonomous X/Y eye motion
  // Periodically initiates motion to a new random point, random speed,
  // holds there for random period until next motion.

  static boolean eyeInMotion = false;
  static int16_t eyeOldX = 512, eyeOldY = 512, eyeCurX = 512, eyeCurY = 512,
                 eyeNewX = 512, eyeNewY = 512;
  static uint32_t eyeMoveStartTime = 0L;
  static int32_t eyeMoveDuration = 0L;

#if CONTROLLABLE
  serEyeCtrl = gazeCmdActive ? 1 : 0;
  if (gazeCmdPending) { // a target from the console or the API
    gazeCmdPending = false;
    eyeOldX = eyeCurX; // glide from wherever the eye is now
    eyeOldY = eyeCurY;
    eyeNewX = gazeCmdX;
    eyeNewY = gazeCmdY;
    eyeMoveDuration = 150000; // ~0.15 s
    eyeMoveStartTime = t;
    eyeInMotion = true;
  }
#endif

  int32_t dt = t - eyeMoveStartTime; // uS elapsed since last eye event

  if (eyeInMotion) {             // Currently moving?
    if (dt >= eyeMoveDuration) { // Time up?  Destination reached.
      if (serEyeCtrl) { // If serial controlled, we're done moving, but stay in
                        // motion
        eyeX = eyeOldX = eyeNewX; // Save position
        eyeY = eyeOldY = eyeNewY;
      } else {
        eyeInMotion = false;               // Stop moving
        eyeMoveDuration = random(3000000); // 0-3 sec stop
        eyeMoveStartTime = t;              // Save initial time of stop
        eyeX = eyeOldX = eyeNewX;          // Save position
        eyeY = eyeOldY = eyeNewY;
      }
    } else { // Move time's not yet fully elapsed -- interpolate position
      int16_t e = ease[255 * dt / eyeMoveDuration] + 1;   // Ease curve
      eyeX = eyeOldX + (((eyeNewX - eyeOldX) * e) / 256); // Interp X
      eyeY = eyeOldY + (((eyeNewY - eyeOldY) * e) / 256); // and Y
    }
  } else { // Eye stopped
    eyeX = eyeOldX;
    eyeY = eyeOldY;
    if (dt > eyeMoveDuration) { // Time up?  Begin new move.
      int16_t dx, dy;
      uint32_t d;
      do { // Pick new dest in circle
        eyeNewX = random(1024);
        eyeNewY = random(1024);
        dx = (eyeNewX * 2) - 1023;
        dy = (eyeNewY * 2) - 1023;
      } while ((d = (dx * dx + dy * dy)) > (1023 * 1023)); // Keep trying
      eyeMoveDuration = random(72000, 144000); // ~1/14 - ~1/7 sec
      eyeMoveStartTime = t;                    // Save initial time of move
      eyeInMotion = true;                      // Start move on next frame
    }
  }
  eyeCurX = eyeX;
  eyeCurY = eyeY;

  // Blinking
#if AUTOBLINK
  // Similar to the autonomous eye movement above -- blink start times
  // and durations are random (within ranges).
  if ((t - timeOfLastBlink) >= timeToNextBlink) { // Start new blink?
    timeOfLastBlink = t;
    uint32_t blinkDuration = random(36000, 72000); // ~1/28 - ~1/14 sec
    // Set up durations for both eyes (if not already winking)
    for (uint8_t e = 0; e < NUM_EYES; e++) {
      if (eye[e].blink.state == NOBLINK) {
        eye[e].blink.state = ENBLINK;
        eye[e].blink.startTime = t;
        eye[e].blink.duration = blinkDuration;
      }
    }
    timeToNextBlink = blinkDuration * 3 + random(4000000);
  }
#endif

  if (eye[eyeIndex].blink.state) { // Eye currently blinking?
    // Check if current blink state time has elapsed
    if ((t - eye[eyeIndex].blink.startTime) >= eye[eyeIndex].blink.duration) {
      // No buttons, or other state...
      if (++eye[eyeIndex].blink.state > DEBLINK) { // Deblinking finished?
        eye[eyeIndex].blink.state = NOBLINK;       // No longer blinking
      } else { // Advancing from ENBLINK to DEBLINK mode
        eye[eyeIndex].blink.duration *= 2; // DEBLINK is 1/2 ENBLINK speed
        eye[eyeIndex].blink.startTime = t;
      }
    }
  }

  // Process motion, blinking and iris scale into renderable values

  // Iris scaling: remap from the 0-1023 scale to iris map height pixel units
  iScale = ((IRIS_MAP_HEIGHT + 1) * 1024) /
           (1024 - (iScale * (IRIS_MAP_HEIGHT - 1) / IRIS_MAP_HEIGHT));
#if CONTROLLABLE
  // Outranks the dilation override: with no pupil there is nothing to
  // dilate.  After the remap, because PUPIL_OFF_SCALE is in drawEye()'s
  // units: through the remap even 0 comes out above it, and leaves a dot.
  if (!pupilOn)
    iScale = PUPIL_OFF_SCALE;
#endif

  // Scale eye X/Y positions (0-1023) to pixel units used by drawEye()
  eyeX = map(eyeX, 0, 1023, 0, SCLERA_WIDTH - 128);
  eyeY = map(eyeY, 0, 1023, 0, SCLERA_HEIGHT - 128);
  // Horizontal position is offset so that eyes are very slightly crossed
  // to appear fixated (converged) at a conversational distance.  Number
  // here was extracted from my posterior and not mathematically based.
  // I suppose one could get all clever with a range sensor, but for now...
  eyeX += 4;
  if (eyeX > (SCLERA_WIDTH - 128))
    eyeX = (SCLERA_WIDTH - 128);

  // Eyelids are rendered using a brightness threshold image.  This same
  // map can be used to simplify another problem: making the upper eyelid
  // track the pupil (eyes tend to open only as much as needed -- e.g. look
  // down and the upper eyelid drops).  Just sample a point in the upper
  // lid map slightly above the pupil to determine the rendering threshold.
  static uint8_t uThreshold = 128;
  uint8_t lThreshold, n;
#if TRACKING
  int16_t sampleX = SCLERA_WIDTH / 2 - (eyeX / 2), // Reduce X influence
      sampleY = SCLERA_HEIGHT / 2 - (eyeY + IRIS_HEIGHT / 4);
  // Eyelid is slightly asymmetrical, so two readings are taken, averaged
  if (sampleY < 0)
    n = 0;
  else
    n = (upper[sampleY][sampleX] + upper[sampleY][SCREEN_WIDTH - 1 - sampleX]) /
        2;
  uThreshold = (uThreshold * 3 + n) / 4; // Filter/soften motion
  // Lower eyelid doesn't track the same way, but seems to be pulled upward
  // by tension from the upper lid.
  lThreshold = 254 - uThreshold;
#else // No tracking -- eyelids full open unless blink modifies them
  uThreshold = lThreshold = 0;
#endif

  // The upper/lower thresholds are then scaled relative to the current
  // blink position so that blinks work together with pupil tracking.
  if (eye[eyeIndex].blink.state) { // Eye currently blinking?
    uint32_t s = (t - eye[eyeIndex].blink.startTime);
    if (s >= eye[eyeIndex].blink.duration)
      s = 255; // At or past blink end
    else
      s = 255 * s / eye[eyeIndex].blink.duration; // Mid-blink
    s = (eye[eyeIndex].blink.state == DEBLINK) ? 1 + s : 256 - s;
    n = (uThreshold * s + 254 * (257 - s)) / 256;
    lThreshold = (lThreshold * s + 254 * (257 - s)) / 256;
  } else {
    n = uThreshold;
  }

  // Pass all the derived values to the eye-rendering function:
#if NETWORK
  // An update has landed and the board is seconds from rebooting into it.
  // Drawing an eye over the "DONE" card would only be confusing, and there is
  // nothing to be gained by rendering a frame we are about to throw away.
  if (webRebootPending())
    return;

  // The address cards own the panels until their deadline passes.
  if (netShowUntil) {
    if ((int32_t)(millis() - netShowUntil) < 0)
      return;
    netShowUntil = 0;
  }
#endif

#if STARTUP_SPLASH
  if (splashPoll()) // the name cards, likewise
    return;
#endif

#if SLEEP
  // Lowest priority of everything that can own the panels: the cards and the
  // splash above have already had their chance, and an address asked for at
  // three in the morning is still worth showing.
  dimmerSetSleepFactor(sleepPoll());
#endif
  // After the cards too: they light the panels themselves, and this fades
  // back from wherever they left them.
  if (dimmerPoll())
    return; // faded all the way off: dark, and nothing to draw
#if DEBUG || CONTROLLABLE
  // Counted here rather than at the top of the function, so the rate is
  // frames drawn and not frames attempted: the returns above hand the panels
  // to the address cards or the splash, and during those nothing is rendered.
  frames++;
#endif
  drawEye(eyeIndex, iScale, eyeX, eyeY, n, lThreshold);
}

// AUTONOMOUS IRIS SCALING (if no photocell or dial) -----------------------
// Autonomous iris motion uses a fractal behavior to similate both the major
// reaction of the eye plus the continuous smaller adjustments that occur.

uint16_t oldIris = (IRIS_MIN + IRIS_MAX) / 2, newIris;

void split( // Subdivides motion path into two sub-paths w/randomization
    int16_t startValue, // Iris scale value (IRIS_MIN to IRIS_MAX) at start
    int16_t endValue,   // Iris scale value at end
    uint32_t startTime, // micros() at start
    int32_t duration,   // Start-to-end time, in microseconds
    int16_t range) {    // Allowable scale value variance when subdividing

  if (range >= 8) { // Limit subdvision count, because recursion
    range /= 2;     // Split range & time in half for subdivision,
    duration /= 2;  // then pick random center point within range:
    int16_t midValue = (startValue + endValue - range) / 2 + random(range);
    uint32_t midTime = startTime + duration;
    split(startValue, midValue, startTime, duration, range); // First half
    split(midValue, endValue, midTime, duration, range);     // Second half
  } else {      // No more subdivisons, do iris motion...
    int32_t dt; // Time (micros) since start of motion
    int16_t v;  // Interim value
    while ((dt = (micros() - startTime)) < duration) {
      v = startValue + (((endValue - startValue) * dt) / duration);
      if (v < IRIS_MIN)
        v = IRIS_MIN; // Clip just in case
      else if (v > IRIS_MAX)
        v = IRIS_MAX;
      frame(v); // Draw frame w/interim iris scale value
    }
  }
}

// MAIN LOOP -- runs continuously after setup() ----------------------------
// Each pass spends about ten seconds inside split(), which calls frame() for
// every frame drawn.  So frame() is where everything else is serviced -- the
// console, the web server, OTA, the BOOT button -- since anything polled from
// here would wait up to ten seconds for its turn.

void loop() {

  // Autonomous iris scaling -- invoke recursive function

  newIris = random(IRIS_MIN, IRIS_MAX);

  split(oldIris, newIris, micros(), 10000000L, IRIS_MAX - IRIS_MIN);
  oldIris = newIris;
}
