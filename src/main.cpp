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

#include <Adafruit_GFX.h>   // Core graphics lib for Adafruit displays
#include <HardwareSerial.h> // Needed for 2nd serial port on ESP32
#include <Preferences.h>    // NVS-backed settings, part of the ESP32 core
#include <SPI.h>

// DEBUG OUTPUT ------------------------------------------------------------
// Set DEBUG to 0 to compile out all serial diagnostics (no code, no strings,
// and Serial is never opened).  DEBUG_BAUD feeds Serial.begin() here and must
// be kept in sync with monitor_speed in platformio.ini.

#define DEBUG 1
#define DEBUG_BAUD 115200
// On-board user LED of the DOIT ESP32 DevKit V1, silkscreened "D2".
// Not broken out to a header pin and unused by the eyes, so it is free.
#define DEBUG_LED_PIN 2

#if DEBUG
#define DEBUG_BEGIN() Serial.begin(DEBUG_BAUD)
#define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define DEBUG_BEGIN()
#define DEBUG_PRINTF(...)
#endif

// STARTUP SPLASH ----------------------------------------------------------
// Names each panel on screen at boot, counting down, so you can tell which
// physical display is on which chip select without tracing wires.  Set to 0
// to boot straight into the eyes.

#define STARTUP_SPLASH 1
#define SPLASH_SECONDS 5

// COMMAND CONSOLE ---------------------------------------------------------
// A line-oriented console on the USB serial port -- the same cable that
// powers the board -- so the eyes can be driven once the head is assembled
// and the BOOT button is out of reach.  Type "help" in the serial monitor.

#define COMMANDS 1

// CLOCK FACE --------------------------------------------------------------
// Turns the iris into an analogue clock.  There is no real time source yet,
// so it free-runs from millis() and the time is set from the console; `clock
// rate` speeds it up to see the hands move.
//
// Hands are found from the polar table the renderer already reads: the high
// 9 bits are the angle and the low 7 the distance, so a pixel is on a hand
// when its angle is near the hand's and it lies within the hand's length.
// No trigonometry and no mask buffer -- two comparisons per hand.

#define CLOCK 1

// 12 o'clock is 128 in the polar table's 0-511 angle, increasing clockwise.
#define CLOCK_NOON 128

// Lengths and half-widths in pixels, measured from the iris centre.  The
// iris radius is IRIS_WIDTH / 2, so 40.
#define CLOCK_HOUR_LEN 20
#define CLOCK_HOUR_HW 2
#define CLOCK_MIN_LEN 30
#define CLOCK_MIN_HW 1
#define CLOCK_SEC_LEN 35
#define CLOCK_SEC_HW 0

// Starting colours, changeable at runtime with `clock color`.  Black reads
// as a silhouette on a bright iris, but is invisible over the pupil, which
// is itself black -- try a light colour on dark eyes.
#define CLOCK_HOUR_COLOR 0x000000
#define CLOCK_MIN_COLOR 0x000000
#define CLOCK_SEC_COLOR 0x000000

// The iris is drawn where iScale * distance / 128 < 64, and distance peaks
// at 127 in the centre, so any scale at or below 64 keeps every pixel in
// the iris and the pupil disappears.  That matters: the pupil is black and
// fills most of the disc, so hands drawn across it are swallowed whole.
// Opening it out turns the iris into a proper dial.
#define CLOCK_FACE_SCALE 64

// BOOT button.  Grounded when pressed, external pull-up on the board.
// GPIO0 is a strapping pin, but only during reset; reading it afterwards is
// fine.  Not broken out to a header on the 30-pin DevKit -- the button is
// the only access.
#define BOOT_BUTTON_PIN 0

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
const uint16_t (*sclera)[SCLERA_WIDTH];
const uint8_t (*upper)[SCREEN_WIDTH];
const uint8_t (*lower)[SCREEN_WIDTH];
const uint16_t (*polar)[80];
const uint16_t (*iris)[IRIS_MAP_WIDTH];

// Registry of the designs compiled in.  Order here is the order the console
// reports and indexes them by.
typedef struct {
  const char *name;
  const uint16_t (*sclera)[SCLERA_WIDTH];
  const uint8_t (*upper)[SCREEN_WIDTH];
  const uint8_t (*lower)[SCREEN_WIDTH];
  const uint16_t (*polar)[80];
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

#define NUM_EYE_DESIGNS (sizeof(eyeDesigns) / sizeof(eyeDesigns[0]))

static uint8_t eyeDesign = 0;

// Panel assignment.  Swapping the chip-select pins moves everything that
// belongs to an eye -- its mirrored eyelids and its splash label -- to the
// other physical panel, which is what makes this a real fix for a miswire
// rather than a cosmetic one.
static bool eyesSwapped = false;
static bool swapPending = false;

#if CLOCK

static bool clockOn = false;
static bool clockSeconds = true;
static bool clockPupil = false; // keep the pupil instead of a full dial
static uint16_t clockRate = 1;              // 1 = real time
static uint32_t clockBaseSec = 10 * 3600UL + 10 * 60UL; // 10:10, watch-ad time
static uint32_t clockBaseMs = 0;
static uint16_t clockHourAng, clockMinAng, clockSecAng;

// [0] hour, [1] minute, [2] second.  The 24-bit copies are kept only so
// `clock` can report what was asked for rather than the lossy 565 value.
static uint32_t clockRGB[3] = {CLOCK_HOUR_COLOR, CLOCK_MIN_COLOR,
                              CLOCK_SEC_COLOR};
static uint16_t clockPix[3];

static inline uint16_t rgb24to565(uint32_t v) {
  return (uint16_t)(((v >> 8) & 0xF800) | ((v >> 5) & 0x07E0) |
                    ((v >> 3) & 0x001F));
}

static void clockSetColor(uint8_t which, uint32_t rgb) {
  clockRGB[which] = rgb & 0xFFFFFF;
  clockPix[which] = rgb24to565(clockRGB[which]);
}

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

static void clockUpdate(void) {
  uint32_t elapsed = ((millis() - clockBaseMs) / 1000UL) * clockRate;
  uint32_t t = (clockBaseSec + elapsed) % 86400UL;
  uint32_t sec = t % 60, min = (t / 60) % 60, hr = (t / 3600) % 12;
  clockSecAng = (uint16_t)((CLOCK_NOON + sec * 512UL / 60UL) % 512UL);
  clockMinAng =
      (uint16_t)((CLOCK_NOON + (min * 60UL + sec) * 512UL / 3600UL) % 512UL);
  clockHourAng = (uint16_t)(
      (CLOCK_NOON + (hr * 3600UL + min * 60UL + sec) * 512UL / 43200UL) % 512UL);
  clockDirs();
}

static uint32_t clockNow(void) {
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

static void saveSettings(void) {
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.putString(PREFS_KEY_EYE, eyeDesigns[eyeDesign].name);
  prefs.putBool(PREFS_KEY_SWAP, eyesSwapped);
  prefs.end();
  settingsDirty = false;
}

static void forgetSettings(void) {
  prefs.begin(PREFS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
  settingsDirty = false;
}

static void setEyeDesign(uint8_t idx) {
  if (idx >= NUM_EYE_DESIGNS)
    idx = 0;
  const EyeDesign *d = &eyeDesigns[idx];
  sclera = d->sclera;
  upper = d->upper;
  lower = d->lower;
  polar = d->polar;
  iris = d->iris;
  eyeDesign = idx;
#if COMMANDS
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
#define SSD1327_SPI_HZ 8000000
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
#endif

#define DISPLAY_DC 33    // Data/command pin for BOTH displays
#define DISPLAY_RESET 27 // Reset pin for BOTH displays
// NOTE: these names are the viewer's left/right, not Frank's.  Verified on
// the bench: D15 drives the panel on the viewer's left, which is FRANK'S
// RIGHT eye; D4 drives Frank's left.  Kept as-is to match the README, but
// see showSplash() for the labels that are correct from Frank's side.
#define SELECT_L_PIN 15  // viewer's left  = Frank's RIGHT eye
#define SELECT_R_PIN 04  // viewer's right = Frank's LEFT eye
#define UART_RX_PIN 13   // Pin to receive UART commands from controller

// INPUT CONFIG (for eye motion -- enable or comment out as needed) --------

#define TRACKING    // If enabled, eyelid tracks pupil
#define IRIS_SMOOTH // If enabled, filter input from IRIS_PIN
#define IRIS_MIN                                                               \
  150 // Clip lower analogRead() range from IRIS_PIN (WAS: 120) - Reduced range
      // so that it doesn't look to odd with multiple eye pairs
#define IRIS_MAX                                                               \
  400 // Clip upper "                                (WAS: 720) - Reduced range
      // so that it doesn't look to odd with multiple eye pairs
#define AUTOBLINK // If enabled, eyes blink autonomously

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

SPISettings settings(16000000, MSBFIRST,
                     SPI_MODE3); // 26.667MHz seems reliable on the ESP32.
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

// Called between frames only: a swap landing mid-transaction would leave a
// chip select asserted on the wrong panel.
static void applySwap(void) {
  uint8_t a = eye[0].cs, b = eye[1].cs;
  eye[0].cs = b;
  eye[1].cs = a;
  eye[0].display.setCS((int8_t)b);
  eye[1].display.setCS((int8_t)a);
}

// INITIALIZATION -- runs once at startup ----------------------------------

#if COMMANDS
static void loadSettings(void); // defined with the console, below setup()
#endif

HardwareSerial SerialIn(1);

#if STARTUP_SPLASH

// The default GFX font is a 6x8 cell, so a string's width is just its
// length scaled up.
static void splashCenter(GFXcanvas1 &c, const char *str, uint8_t size,
                         int16_t y) {
  c.setTextSize(size);
  c.setCursor((SCREEN_WIDTH - (int16_t)strlen(str) * 6 * size) / 2, y);
  c.print(str);
}

// Left and right are given from FRANK'S OWN perspective, the way anatomy
// is always described: facing him, his right eye is the one on your left.
//
// Which panel that is depends on wiring, and nothing in the sketch or the
// README says whose perspective SELECT_L_PIN / SELECT_R_PIN were named
// from.  The splash prints the chip-select pin alongside the label so the
// mapping can be read off the panels once and settled here for good.
static void showSplash(void) {
  // One 1-bit canvas serves both panel types: 2 KB, versus 32 KB for a
  // colour one, and the text is monochrome either way.
  GFXcanvas1 canvas(SCREEN_WIDTH, SCREEN_HEIGHT);

  // Confirmed on the bench: the panel on SELECT_L_PIN (D15) is the one on
  // FRANK'S RIGHT -- the viewer's left.  So the upstream L/R pin names are
  // viewer-relative, and eye[0] is Frank's right eye.  Both perspectives are
  // shown because every previous attempt to write this down was ambiguous.
  static const char *const franksSide[2] = {"RIGHT", "LEFT"};
  static const char *const yourSide[2] = {"LEFT", "RIGHT"};
#if USE_SSD1327
  static uint8_t splashBuf[SSD1327_FRAME_BYTES];
#endif

  DEBUG_PRINTF("[creeper-eyes] splash: naming panels for %d s\n",
               SPLASH_SECONDS);

  for (int8_t remain = SPLASH_SECONDS; remain > 0; remain--) {
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

#if USE_SSD1327
      // 1 bit per pixel out, 4 bits per pixel in, two pixels to a byte.
      uint16_t o = 0;
      for (int16_t y = 0; y < SCREEN_HEIGHT; y++)
        for (int16_t x = 0; x < SCREEN_WIDTH; x += 2, o++)
          splashBuf[o] = (uint8_t)((canvas.getPixel(x, y) ? 0xF0 : 0x00) |
                                   (canvas.getPixel(x + 1, y) ? 0x0F : 0x00));
      SPI.beginTransaction(graySPI);
      eye[e].display.pushFrame(splashBuf);
      SPI.endTransaction();
#else
      eye[e].display.drawBitmap(0, 0, canvas.getBuffer(), SCREEN_WIDTH,
                                SCREEN_HEIGHT, 0xFFFF, 0x0000);
#endif
    }
    delay(1000);
  }

  // Hand a clean screen to the eyes.
  for (uint8_t e = 0; e < NUM_EYES; e++) {
#if USE_SSD1327
    eye[e].display.fill(graySPI, 0x0);
#else
    eye[e].display.fillScreen(0x0000);
#endif
  }
}

#endif // STARTUP_SPLASH

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
  // SerialIn.begin(9600, SERIAL_8N1, UART_RX_PIN); // disabled
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
    eye[e].display.begin();
    digitalWrite(eye[e].cs, HIGH); // Deselect
#endif
  }
  DEBUG_PRINTF("[creeper-eyes] %u panel(s) initialised (%s)\n",
               (unsigned)NUM_EYES, USE_SSD1327 ? "SSD1327 grey" : "SSD1351 rgb");

  // Eyelid mirroring for the left eye is done in software, in drawEye(), so
  // it behaves the same on both panel types.  The hardware alternative below
  // mirrors the whole SSD1351 panel in its controller -- which would also
  // mirror gaze direction and cross the eyes, so leave it off.
  // eye[0].display.writeCommand(SSD1351_CMD_SETREMAP);
  // eye[0].display.write16(0x76);

#if COMMANDS
  loadSettings(); // before the splash, so its labels are correct
#endif
#if STARTUP_SPLASH
  showSplash();
#endif
#if COMMANDS
  Serial.println(F("[creeper-eyes] console ready -- type 'help'"));
#endif
}

// EYE-RENDERING FUNCTION --------------------------------------------------9
void drawEye(        // Renders one eye.  Inputs must be pre-clipped & valid.
    uint8_t e,       // Eye array index; 0 or 1 for left/right
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
  static uint16_t pBurst[SCREEN_WIDTH *
                         SCREEN_HEIGHT]; // Full frame buffer possible on ESP32

  // Set up raw pixel dump to entire screen.  Although such writes can wrap
  // around automatically from end of rect back to beginning, the region is
  // reset on each frame here in case of an SPI glitch.

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
  if (clockOn) {
    const uint8_t hlen[3] = {CLOCK_HOUR_LEN, CLOCK_MIN_LEN, CLOCK_SEC_LEN};
    const uint8_t hhw[3] = {CLOCK_HOUR_HW, CLOCK_MIN_HW, CLOCK_SEC_HW};
    const int16_t cx = (int16_t)(IRIS_WIDTH / 2) - irisOriginX;
    const int16_t cy = (int16_t)(IRIS_HEIGHT / 2) - irisOriginY;

    // Hour first so the minute and second hands lie over it.
    for (uint8_t h = 0; h < 3; h++) {
      if (h == 2 && !clockSeconds)
        continue;
      const int16_t ux = clockDirX[h], uy = clockDirY[h];
      const int16_t px = (int16_t)-uy, py = ux; // perpendicular
      for (int16_t i = 0; i <= (int16_t)hlen[h]; i++) {
        for (int16_t j = -(int16_t)hhw[h]; j <= (int16_t)hhw[h]; j++) {
          int16_t sx = cx + (int16_t)((ux * i + px * j) >> 8);
          int16_t sy = cy + (int16_t)((uy * i + py * j) >> 8);
          if (sx < 0 || sx >= SCREEN_WIDTH || sy < 0 || sy >= SCREEN_HEIGHT)
            continue;
          int16_t ix = irisOriginX + sx, iy = irisOriginY + sy;
          if (ix < 0 || ix >= IRIS_WIDTH || iy < 0 || iy >= IRIS_HEIGHT)
            continue;
          if ((polar[iy][ix] & 0x7F) >= 127)
            continue; // outside the iris circle
          uint8_t lx = mirrorLids ? (uint8_t)(SCREEN_WIDTH - 1 - sx) : (uint8_t)sx;
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
  static uint8_t gBurst[SSD1327_FRAME_BYTES];
  for (uint16_t i = 0, o = 0; i < SCREEN_WIDTH * SCREEN_HEIGHT; i += 2, o++)
    gBurst[o] = (uint8_t)((rgb565ToGray4(pBurst[i]) << 4) |
                          rgb565ToGray4(pBurst[i + 1]));

  SPI.beginTransaction(graySPI);
  eye[e].display.pushFrame(gBurst);
  SPI.endTransaction();
#else
  eye[e].display.startWrite();
  eye[e].display.writeCommand(SSD1351_CMD_SETROW); // Y range
  eye[e].display.write16(0x0);
  eye[e].display.write16(SCREEN_HEIGHT - 1);
  eye[e].display.writeCommand(SSD1351_CMD_SETCOLUMN); // X range
  eye[e].display.write16(0x0);
  eye[e].display.write16(SCREEN_WIDTH - 1);
  eye[e].display.writeCommand(SSD1351_CMD_WRITERAM); // Begin write

  // For ESP32, use writePixels function to transfer the whole framebuffer in
  // one large burst
  SPI.writePixels((uint8_t *)pBurst, sizeof(pBurst));
  eye[e].display.endWrite();
#endif
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

#ifdef AUTOBLINK
uint32_t timeOfLastBlink = 0L, timeToNextBlink = 0L;
#endif

#if COMMANDS

// These five are pointers TO const data, not const pointers, so the whole
// eye can be swapped at runtime -- which is exactly what the note above
// them describes.  Swap between frames, never mid-render: drawEye() reads
// all five as it scans, so changing them under it would tear one frame.
// Returns NUM_EYE_DESIGNS if there is no match.
static uint8_t eyeDesignByName(const char *name) {
  for (uint8_t i = 0; i < NUM_EYE_DESIGNS; i++)
    if (!strcmp(name, eyeDesigns[i].name))
      return i;
  return NUM_EYE_DESIGNS;
}

// Runs before the splash, so the labels reflect a restored swap.
static void loadSettings(void) {
  prefs.begin(PREFS_NAMESPACE, true); // read-only
  String saved = prefs.getString(PREFS_KEY_EYE, "");
  bool sw = prefs.getBool(PREFS_KEY_SWAP, false);
  prefs.end();

  if (sw) {
    eyesSwapped = true;
    applySwap();
  }
  if (saved.length()) {
    uint8_t idx = eyeDesignByName(saved.c_str());
    if (idx < NUM_EYE_DESIGNS) {
      setEyeDesign(idx);
    } else {
      DEBUG_PRINTF("[creeper-eyes] saved eye '%s' is not in this build, "
                   "using %s\n",
                   saved.c_str(), eyeDesigns[0].name);
    }
  }
  DEBUG_PRINTF("[creeper-eyes] settings: eye=%s swap=%s\n",
               eyeDesigns[eyeDesign].name, eyesSwapped ? "yes" : "no");
}

static void listEyeDesigns(void) {
  for (uint8_t i = 0; i < NUM_EYE_DESIGNS; i++)
    Serial.printf("  %u  %-10s%s\n", (unsigned)i, eyeDesigns[i].name,
                  i == eyeDesign ? "  <- current" : "");
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

#define STARTLE_WINDUP_MS 1400 // slow constrict -- the tension
#define STARTLE_HOLD_MS 1200   // eyes held wide after the jolt

static void startleBegin(void) {
  startleWasAuto = !dilateCmdActive; // so we can hand back what we took
  startleWasPct = dilateCmdPct;
  setDilation(0);      // constrict to a pinpoint
  dilateEaseDiv = 48;  // ...slowly
  startleMark = millis();
  startleState = STARTLE_WINDUP;
}

static void startleCancel(void) {
  startleState = STARTLE_OFF;
  dilateEaseDiv = 8;
}

static void pollStartle(void) {
  if (startleState == STARTLE_OFF)
    return;

  uint32_t now = millis();

  if (startleState == STARTLE_WINDUP) {
    if (now - startleMark >= STARTLE_WINDUP_MS) {
      setDilation(100);            // full open
      dilateCurrent = dilateCmdValue; // ...instantly, no ease
      dilateEaseDiv = 8;
#ifdef AUTOBLINK
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

static void cmdHelp(void) {
  Serial.print(F("\ncommands:\n"
                 "  eye                       list the designs built in\n"
                 "  eye <name>|<index>|next   select an eye design\n"
                 "  look <x> <y>              aim gaze, 0-1023 each "
                 "(512 512 = centre)\n"
                 "  look auto                 return to autonomous motion\n"
                 "  dilate <0-100>            pupil width, 100 = fully "
                 "dilated\n"
                 "  dilate auto               return to autonomous dilation\n"
                 "  startle                   constrict, then snap wide "
                 "with a blink\n"
                 "  clock [on|off|set|rate]   analogue clock in the iris\n"
                 "  swap [on|off]             swap which panel is which "
                 "eye\n"
                 "  save                      remember eye and swap "
                 "across reboots\n"
                 "  forget                    clear saved settings\n"
                 "  blink                     blink both eyes now\n"
                 "  splash                    re-show the panel name cards\n"
                 "  status                    report current state\n"
                 "  help                      this list\n"));
}

static void cmdStatus(void) {
  Serial.printf("eye=%u/%u %s gaze=%s", (unsigned)eyeDesign,
                (unsigned)NUM_EYE_DESIGNS, eyeDesigns[eyeDesign].name,
                gazeCmdActive ? "commanded" : "auto");
  if (gazeCmdActive)
    Serial.printf("(%d,%d)", gazeCmdX, gazeCmdY);
  Serial.printf(" dilate=%s", dilateCmdActive ? "" : "auto");
  if (dilateCmdActive)
    Serial.printf("%u%%", (unsigned)dilateCmdPct);
  if (startleState != STARTLE_OFF)
    Serial.print(startleState == STARTLE_WINDUP ? " startle=windup"
                                                : " startle=hold");
  Serial.printf(" swap=%s%s", eyesSwapped ? "on" : "off",
                settingsDirty ? " (unsaved)" : "");
  Serial.printf(" panel=%s heap=%u up=%us",
                USE_SSD1327 ? "ssd1327" : "ssd1351",
                (unsigned)ESP.getFreeHeap(), (unsigned)(millis() / 1000));
#if DEBUG
  Serial.printf(" fps=%u", lastFps);
#endif
  Serial.println();
}

static void handleCommand(char *line) {
  char *cmd = strtok(line, " \t");
  if (!cmd)
    return;
  for (char *c = cmd; *c; c++)
    *c = (char)tolower((unsigned char)*c);

  if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
    cmdHelp();
  } else if (!strcmp(cmd, "status")) {
    cmdStatus();
  } else if (!strcmp(cmd, "eye")) {
    char *arg = strtok(NULL, " \t");
    if (arg)
      for (char *c = arg; *c; c++)
        *c = (char)tolower((unsigned char)*c);
    if (!arg || !strcmp(arg, "list")) { // bare "eye" reports what is available
      listEyeDesigns();
      return;
    }
    if (!strcmp(arg, "next") || !strcmp(arg, "toggle")) {
      setEyeDesign((uint8_t)((eyeDesign + 1) % NUM_EYE_DESIGNS));
    } else if (arg[0] >= '0' && arg[0] <= '9') { // by index
      long idx = atol(arg);
      if (idx < 0 || idx >= (long)NUM_EYE_DESIGNS) {
        Serial.printf("err: no design %ld -- %u built in\n", idx,
                      (unsigned)NUM_EYE_DESIGNS);
        return;
      }
      setEyeDesign((uint8_t)idx);
    } else { // by name
      uint8_t idx = eyeDesignByName(arg);
      if (idx >= NUM_EYE_DESIGNS) {
        Serial.printf("err: no design '%s'. built in:\n", arg);
        listEyeDesigns();
        return;
      }
      setEyeDesign(idx);
    }
    Serial.printf("ok eye=%u %s\n", (unsigned)eyeDesign,
                  eyeDesigns[eyeDesign].name);
  } else if (!strcmp(cmd, "look")) {
    char *a1 = strtok(NULL, " \t");
    if (!a1) {
      Serial.println(F("usage: look <0-1023> <0-1023> | look auto"));
      return;
    }
    if (!strcmp(a1, "auto")) {
      gazeCmdActive = false;
      Serial.println(F("ok gaze=auto"));
      return;
    }
    char *a2 = strtok(NULL, " \t");
    if (!a2) {
      Serial.println(F("usage: look <0-1023> <0-1023> | look auto"));
      return;
    }
    long x = atol(a1), y = atol(a2);
    if (x < 0 || x > 1023 || y < 0 || y > 1023) {
      Serial.println(F("err: both values must be 0-1023"));
      return;
    }
    gazeCmdX = (int16_t)x;
    gazeCmdY = (int16_t)y;
    gazeCmdActive = true;
    gazeCmdPending = true;
    Serial.printf("ok gaze=(%ld,%ld)\n", x, y);
#if CLOCK
  } else if (!strcmp(cmd, "clock")) {
    char *arg = strtok(NULL, " \t");
    if (!arg) {
      uint32_t t = clockNow();
      Serial.printf("clock %s %02u:%02u:%02u rate=%ux seconds=%s\n",
                    clockOn ? "on" : "off", (unsigned)(t / 3600),
                    (unsigned)((t / 60) % 60), (unsigned)(t % 60),
                    (unsigned)clockRate, clockSeconds ? "on" : "off");
      Serial.printf("  pupil=%s\n", clockPupil ? "on" : "off (full dial)");
      Serial.printf("  colours hour=%06lX min=%06lX sec=%06lX\n",
                    (unsigned long)clockRGB[0], (unsigned long)clockRGB[1],
                    (unsigned long)clockRGB[2]);
      return;
    }
    for (char *c = arg; *c; c++)
      *c = (char)tolower((unsigned char)*c);

    if (!strcmp(arg, "on") || !strcmp(arg, "off")) {
      clockOn = !strcmp(arg, "on");
      Serial.printf("ok clock=%s\n", clockOn ? "on" : "off");
    } else if (!strcmp(arg, "set")) {
      char *v = strtok(NULL, " \t");
      unsigned h = 0, m = 0, sec = 0;
      if (!v || sscanf(v, "%u:%u:%u", &h, &m, &sec) < 2) {
        Serial.println(F("usage: clock set HH:MM[:SS]"));
        return;
      }
      if (h > 23 || m > 59 || sec > 59) {
        Serial.println(F("err: out of range"));
        return;
      }
      clockSet(h * 3600UL + m * 60UL + sec);
      Serial.printf("ok clock set %02u:%02u:%02u\n", h, m, sec);
    } else if (!strcmp(arg, "rate")) {
      char *v = strtok(NULL, " \t");
      long r = v ? atol(v) : 0;
      if (r < 1 || r > 3600) {
        Serial.println(F("usage: clock rate <1-3600>"));
        return;
      }
      clockSet(clockNow()); // rebase so the jump is not retroactive
      clockRate = (uint16_t)r;
      Serial.printf("ok clock rate=%ldx\n", r);
    } else if (!strcmp(arg, "color") || !strcmp(arg, "colour")) {
      char *a = strtok(NULL, " \t");
      char *b = strtok(NULL, " \t");
      if (!a) {
        Serial.println(F("usage: clock color [hour|min|sec] RRGGBB"));
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
          Serial.println(F("usage: clock color [hour|min|sec] RRGGBB"));
          return;
        }
        hex = b;
      }
      if (*hex == '#')
        hex++;
      char *endp = NULL;
      unsigned long v = strtoul(hex, &endp, 16);
      if (!endp || *endp || v > 0xFFFFFFUL) {
        Serial.println(F("err: colour must be 6 hex digits, e.g. FF8800"));
        return;
      }
      if (which < 0) {
        for (uint8_t i = 0; i < 3; i++)
          clockSetColor(i, (uint32_t)v);
        Serial.printf("ok clock color all=%06lX\n", v);
      } else {
        clockSetColor((uint8_t)which, (uint32_t)v);
        Serial.printf("ok clock color %s=%06lX\n",
                      which == 0 ? "hour" : which == 1 ? "min" : "sec", v);
      }
    } else if (!strcmp(arg, "pupil")) {
      char *v = strtok(NULL, " \t");
      clockPupil = (v && !strcmp(v, "on"));
      Serial.printf("ok pupil=%s (%s)\n", clockPupil ? "on" : "off",
                    clockPupil ? "hands cut off by the pupil" : "full dial");
    } else if (!strcmp(arg, "secs")) {
      char *v = strtok(NULL, " \t");
      clockSeconds = !(v && !strcmp(v, "off"));
      Serial.printf("ok seconds=%s\n", clockSeconds ? "on" : "off");
    } else {
      Serial.println(
          F("usage: clock [on|off|set HH:MM[:SS]|rate N|secs on|off|"
            "color [hour|min|sec] RRGGBB|pupil on|off]"));
    }
#endif
  } else if (!strcmp(cmd, "swap")) {
    char *arg = strtok(NULL, " \t");
    bool want = !eyesSwapped;
    if (arg) {
      if (!strcmp(arg, "on"))
        want = true;
      else if (!strcmp(arg, "off"))
        want = false;
      else {
        Serial.println(F("usage: swap [on|off]"));
        return;
      }
    }
    if (want != eyesSwapped) {
      eyesSwapped = want;
      swapPending = true; // applied between frames
      settingsDirty = true;
    }
    Serial.printf("ok swap=%s\n", eyesSwapped ? "on" : "off");
  } else if (!strcmp(cmd, "save")) {
    saveSettings();
    Serial.printf("ok saved eye=%s swap=%s\n",
                  eyeDesigns[eyeDesign].name, eyesSwapped ? "on" : "off");
  } else if (!strcmp(cmd, "forget")) {
    forgetSettings();
    Serial.println(F("ok settings cleared; build defaults apply at next boot"));
  } else if (!strcmp(cmd, "startle")) {
    startleBegin();
    Serial.println(F("ok startle"));
  } else if (!strcmp(cmd, "dilate")) {
    startleCancel(); // an explicit width wins over a running effect
    char *arg = strtok(NULL, " \t");
    if (!arg) {
      Serial.println(F("usage: dilate <0-100> | dilate auto"));
      return;
    }
    if (!strcmp(arg, "auto")) {
      dilateCmdActive = false;
      Serial.println(F("ok dilate=auto"));
      return;
    }
    long pct = atol(arg);
    if (pct < 0 || pct > 100) {
      Serial.println(F("err: dilation must be 0-100"));
      return;
    }
    setDilation((uint8_t)pct);
    Serial.printf("ok dilate=%ld%%\n", pct);
#ifdef AUTOBLINK
  } else if (!strcmp(cmd, "blink")) {
    timeToNextBlink = 0; // due immediately on the next frame
    Serial.println(F("ok blink"));
#endif
#if STARTUP_SPLASH
  } else if (!strcmp(cmd, "splash")) {
    showSplash();
    Serial.println(F("ok splash"));
#endif
  } else {
    Serial.printf("unknown command '%s' -- try 'help'\n", cmd);
  }
}

// Non-blocking: called once per rendered frame, never from loop(), which
// spends ~10 s inside split() and would make the console feel dead.
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
        handleCommand(line);
      len = 0;
    } else if (len < sizeof(line) - 1) {
      line[len++] = c;
    } else {
      len = 0; // overlong line, discard rather than truncate
    }
  }
}

static void pollBootButton(void) {
  static bool wasDown = false;
  static uint32_t lastEdge = 0;
  bool isDown = (digitalRead(BOOT_BUTTON_PIN) == LOW);
  uint32_t now = millis();

  if (isDown != wasDown && (now - lastEdge) > 40) { // debounce
    lastEdge = now;
    wasDown = isDown;
    if (isDown) { // act on press, not release
      setEyeDesign((uint8_t)((eyeDesign + 1) % NUM_EYE_DESIGNS));
      Serial.printf("ok eye=%u %s (button)\n", (unsigned)eyeDesign,
                    eyeDesigns[eyeDesign].name);
    }
  }
}

#endif // COMMANDS

void frame(            // Process motion for a single frame of left or right eye
    uint16_t iScale) { // Iris scale (0-1023) passed in
  static uint32_t frames = 0;  // Used in frame rate calculation
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

#if CLOCK
  if (clockOn)
    clockUpdate();
#endif

  if (swapPending) { // between frames, never mid-transaction
    swapPending = false;
    applySwap();
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

#if CLOCK
  // Open the pupil right out so the whole iris reads as a dial.  Applied
  // after the dilation override, which it deliberately outranks.
  if (clockOn && !clockPupil)
    iScale = CLOCK_FACE_SCALE;
#endif

#if DEBUG
  // Heartbeat: proves the render loop is alive even with no displays wired.
  {
    static uint32_t lastReport = 0;
    uint32_t now = millis();
    frames++;
    if (now - lastReport >= 1000) {
      DEBUG_PRINTF("[creeper-eyes] fps=%u heap=%u\n", (unsigned)frames,
                   (unsigned)ESP.getFreeHeap());
      digitalWrite(DEBUG_LED_PIN, !digitalRead(DEBUG_LED_PIN));
#if COMMANDS
      lastFps = (uint16_t)frames;
#endif
      frames = 0;
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

#if COMMANDS
  serEyeCtrl = gazeCmdActive ? 1 : 0;
  if (gazeCmdPending) { // new target from the console
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
#ifdef AUTOBLINK
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

  // Iris scaling: remap from 0-1023 input to iris map height pixel units
  iScale = ((IRIS_MAP_HEIGHT + 1) * 1024) /
           (1024 - (iScale * (IRIS_MAP_HEIGHT - 1) / IRIS_MAP_HEIGHT));

  // Scale eye X/Y positions (0-1023) to pixel units used by drawEye()
  eyeX = map(eyeX, 0, 1023, 0, SCLERA_WIDTH - 128);
  eyeY = map(eyeY, 0, 1023, 0, SCLERA_HEIGHT - 128);
  if (eyeIndex ==
      1) { // this inverts the motion of the eyes
           // eyeX = (SCLERA_WIDTH - 128) - eyeX; // Mirrored display
  }

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
#ifdef TRACKING
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

void loop() {

  // Autonomous iris scaling -- invoke recursive function

  newIris = random(IRIS_MIN, IRIS_MAX);

  split(oldIris, newIris, micros(), 10000000L, IRIS_MAX - IRIS_MIN);
  oldIris = newIris;
}
