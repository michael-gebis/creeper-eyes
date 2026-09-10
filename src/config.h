// Build-time configuration -- every switch and tunable in one place.
//
// Each switch is #ifndef-guarded, so platformio.ini can override any of them
// without this file being edited:
//
//   build_flags = -DNETWORK=0 -DCLOCK=0
//
// See the environments in platformio.ini for the combinations that ship.

#ifndef CONFIG_H
#define CONFIG_H

// NETWORK -----------------------------------------------------------------
// Credentials live in include/secrets.h, which is gitignored -- copy
// include/secrets.h.example to get started.  Nothing secret is committed,
// and a clone without it still builds: the include is guarded, and an
// unconfigured board falls through to the setup portal.

#ifndef NETWORK
#define NETWORK 1
#endif

#define WIFI_HOSTNAME "frank"
#define WIFI_AP_NAME "frank-setup"

// How long to wait on a known network before giving up and opening the
// portal, and how long the portal itself stays up before the eyes carry on
// regardless.  A prop with no network should still be a working prop.
#define WIFI_CONNECT_MS 15000
#define WIFI_PORTAL_S 180

// How long `net` leaves the address cards up before the eyes resume.
#define NET_SHOW_MS 12000

// 128 px at 6 px per character in the default font.
#define NET_COLS 21

// Time is taken from NTP with a POSIX TZ string, rather than the manual
// hour/minute offset plus a DST checkbox the wandering-hour-clock used.
// A TZ string carries the DST *rules*, so the changeover happens on its
// own instead of needing a visit twice a year.
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.nist.gov"

// US Pacific by default.  A POSIX TZ string carries the DST *rules*, not
// just an offset -- "PST8PDT" names both standard and summer time, and
// "M3.2.0/2,M11.1.0/2" is the US changeover: second Sunday in March at
// 02:00, first Sunday in November at 02:00.  So DST is not a separate
// setting, and nothing needs touching twice a year.
#define TZ_DEFAULT "PST8PDT,M3.2.0/2,M11.1.0/2"
#define TZ_MAX 48

// Optional: a clone without it still builds, and an unconfigured board
// falls through to the setup portal.
#if defined(__has_include)
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif

// DEBUG OUTPUT ------------------------------------------------------------
// Set DEBUG to 0 to compile out all serial diagnostics (no code, no strings,
// and Serial is never opened).  DEBUG_BAUD feeds Serial.begin() here and must
// be kept in sync with monitor_speed in platformio.ini.

#ifndef DEBUG
#define DEBUG 1
#endif
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

#ifndef STARTUP_SPLASH
#define STARTUP_SPLASH 1
#endif
#define SPLASH_SECONDS 5

// COMMAND CONSOLE ---------------------------------------------------------
// A line-oriented console on the USB serial port -- the same cable that
// powers the board -- so the eyes can be driven once the head is assembled
// and the BOOT button is out of reach.  Type "help" in the serial monitor.

#ifndef COMMANDS
#define COMMANDS 1
#endif


// PUPIL -------------------------------------------------------------------
// The iris is drawn where iScale * distance / 128 < 64, and distance peaks
// at 127 at the centre, so any scale at or below 64 keeps every pixel in
// the iris and the pupil disappears, leaving a full iris disc.  Independent
// of the clock -- see the `pupil` command.
#define PUPIL_OFF_SCALE 64

// CLOCK FACE --------------------------------------------------------------
// Turns the iris into an analogue clock.  There is no real time source yet,
// so it free-runs from millis() and the time is set from the console; `clock
// rate` speeds it up to see the hands move.
//
// Hands are found from the polar table the renderer already reads: the high
// 9 bits are the angle and the low 7 the distance, so a pixel is on a hand
// when its angle is near the hand's and it lies within the hand's length.
// No trigonometry and no mask buffer -- two comparisons per hand.

#ifndef CLOCK
#define CLOCK 1
#endif

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


// BOOT button.  Grounded when pressed, external pull-up on the board.
// GPIO0 is a strapping pin, but only during reset; reading it afterwards is
// fine.  Not broken out to a header on the 30-pin DevKit -- the button is
// the only access.
#define BOOT_BUTTON_PIN 0

#endif // CONFIG_H
