// Build-time configuration.
//
// Each switch is #ifndef-guarded, so platformio.ini can override any of them
// without this file being edited:
//
//   build_flags = -DNETWORK=0 -DCLOCK=0
//
// See the environments in platformio.ini for the combinations that ship.
//
// Everything in this file is overridable; the only names that are not are
// the include guard, CONTROLLABLE (derived from two others), and the two
// FAVICON_* values that FAVICON selects between.
//
// Not quite everything is here.  The switches this project inherited from
// upstream -- USE_SSD1327, SSD1327_SPI_HZ, STARTLE_WINDUP_MS,
// STARTLE_HOLD_MS, TRACKING, AUTOBLINK and IRIS_MIN/IRIS_MAX -- are still
// declared in main.cpp beside the rendering code they belong to.  They are
// overridable from build_flags just the same; USE_SSD1327 is set that way by
// the `gray` environment.

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

#ifndef WIFI_HOSTNAME
#define WIFI_HOSTNAME "frank"
#endif
#ifndef WIFI_AP_NAME
#define WIFI_AP_NAME "frank-setup"
#endif

// The legacy /cmd?c=... endpoint, which runs a console command over HTTP
// and answers in prose.  Handy from a shell, but the JSON API under
// /api/v1 is what programs should use.  Set to 0 to drop it.
#ifndef WEB_CMD_ENDPOINT
#define WEB_CMD_ENDPOINT 1
#endif

// How long to wait on a known network before giving up and opening the
// portal, and how long the portal itself stays up before the eyes carry on
// regardless.  A prop with no network should still be a working prop.
#ifndef WIFI_CONNECT_MS
#define WIFI_CONNECT_MS 15000
#endif
// How long to wait between attempts once the board has given up at boot.
// Watching for a link is not enough on its own: the setup portal tears the
// association down when it times out, so nothing is trying any more and a
// network that comes back would never be noticed.
#ifndef WIFI_RETRY_MS
#define WIFI_RETRY_MS 30000
#endif

#ifndef WIFI_PORTAL_S
#define WIFI_PORTAL_S 60
#endif

// How long `net` leaves the address cards up before the eyes resume.
#ifndef NET_SHOW_MS
#define NET_SHOW_MS 12000
#endif

// 128 px at 6 px per character in the default font.
#ifndef NET_COLS
#define NET_COLS 21
#endif

// Time is taken from NTP with a POSIX TZ string, rather than the manual
// hour/minute offset plus a DST checkbox the wandering-hour-clock used.
// A TZ string carries the DST *rules*, so the changeover happens on its
// own instead of needing a visit twice a year.
#ifndef NTP_SERVER_1
#define NTP_SERVER_1 "pool.ntp.org"
#endif
#ifndef NTP_SERVER_2
#define NTP_SERVER_2 "time.nist.gov"
#endif

// US Pacific by default.  A POSIX TZ string carries the DST *rules*, not
// just an offset -- "PST8PDT" names both standard and summer time, and
// "M3.2.0/2,M11.1.0/2" is the US changeover: second Sunday in March at
// 02:00, first Sunday in November at 02:00.  So DST is not a separate
// setting, and nothing needs touching twice a year.
#ifndef TZ_DEFAULT
#define TZ_DEFAULT "PST8PDT,M3.2.0/2,M11.1.0/2"
#endif
#ifndef TZ_MAX
#define TZ_MAX 48
#endif

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
#ifndef DEBUG_BAUD
#define DEBUG_BAUD 115200
#endif
// On-board user LED of the DOIT ESP32 DevKit V1, silkscreened "D2".
// Not broken out to a header pin and unused by the eyes, so it is free.
#ifndef DEBUG_LED_PIN
#define DEBUG_LED_PIN 2
#endif

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
#ifndef SPLASH_SECONDS
#define SPLASH_SECONDS 5
#endif

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
#ifndef PUPIL_OFF_SCALE
#define PUPIL_OFF_SCALE 64
#endif

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
#ifndef CLOCK_NOON
#define CLOCK_NOON 128
#endif

// Lengths and half-widths in pixels, measured from the iris centre.  The
// iris radius is IRIS_WIDTH / 2, so 40.
#ifndef CLOCK_HOUR_LEN
#define CLOCK_HOUR_LEN 20
#endif
#ifndef CLOCK_HOUR_HW
#define CLOCK_HOUR_HW 2
#endif
#ifndef CLOCK_MIN_LEN
#define CLOCK_MIN_LEN 30
#endif
#ifndef CLOCK_MIN_HW
#define CLOCK_MIN_HW 1
#endif
#ifndef CLOCK_SEC_LEN
#define CLOCK_SEC_LEN 35
#endif
#ifndef CLOCK_SEC_HW
#define CLOCK_SEC_HW 0
#endif

// Starting colours, changeable at runtime with `clock color`.  Black reads
// as a silhouette on a bright iris, but is invisible over the pupil, which
// is itself black -- try a light colour on dark eyes.
#ifndef CLOCK_HOUR_COLOR
#define CLOCK_HOUR_COLOR 0x000000
#endif
#ifndef CLOCK_MIN_COLOR
#define CLOCK_MIN_COLOR 0x000000
#endif
#ifndef CLOCK_SEC_COLOR
#define CLOCK_SEC_COLOR 0x000000
#endif


// BOOT button.  Grounded when pressed, external pull-up on the board.
// GPIO0 is a strapping pin, but only during reset; reading it afterwards is
// fine.  Not broken out to a header on the 30-pin DevKit -- the button is
// the only access.
#ifndef BOOT_BUTTON_PIN
#define BOOT_BUTTON_PIN 0
#endif

// IDENTITY ------------------------------------------------------------------
// The version is bumped by hand, and only for a feature worth telling
// somebody about -- the commit below already distinguishes every build.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "1.0"
#endif
#ifndef PROJECT_URL
#define PROJECT_URL "https://github.com/michael-gebis/creeper-eyes"
#endif

// Supplied by tools/git_rev.py at build time.  Defaulted here so the project
// still compiles outside a git checkout, or with the extra script removed.
#ifndef GIT_REV
#define GIT_REV "unknown"
#endif
#ifndef GIT_DIRTY
#define GIT_DIRTY 0
#endif

// The commit, with a marker when the tree had uncommitted changes -- an
// unmarked hash is a promise that this binary is that commit, and a build
// from a dirty tree cannot make it.
#if GIT_DIRTY
#define FIRMWARE_COMMIT GIT_REV "+dirty"
#else
#define FIRMWARE_COMMIT GIT_REV
#endif

// BATTERY-BACKED CLOCK ------------------------------------------------------
// An optional DS3231 on I2C -- see docs/WIRING_RTC.md.  Off by default: this
// is an add-on, and a build that does not have one should not carry the code
// or the I2C bus for it.
//
//   build_flags = -DRTC=1
//
// With it on, a missing module is still not an error: the probe at boot finds
// nothing, says so, and the clock free-runs as before.
#ifndef RTC
#define RTC 0
#endif

// The ESP32's default I2C pins, and both unused by the displays.
#ifndef RTC_SDA_PIN
#define RTC_SDA_PIN 21
#endif
#ifndef RTC_SCL_PIN
#define RTC_SCL_PIN 22
#endif

// The DS3231's fixed address.  The AT24C32 EEPROM that shares these boards
// sits at 0x57 and is not used.
#ifndef RTC_ADDR
#define RTC_ADDR 0x68
#endif

// Which tab icon the control page carries.  FAVICON_FRANK is the monster's
// head; FAVICON_EYES is the two panels on their own, for a build going into
// something that is not a Frankenstein.  Both live in src/favicon.h and only
// the chosen one is compiled in.
#define FAVICON_FRANK 0
#define FAVICON_EYES 1
#ifndef FAVICON
#define FAVICON FAVICON_FRANK
#endif

// IPv6.  All of it: bringing the address up, reporting it, and printing it
// on the address cards.  Off, and not yet selectable.
//
// WiFiServer in the ESP32 Arduino core opens an AF_INET socket and nothing
// else, so nothing listens on the board's IPv6 address -- it answers pings
// and refuses HTTP.  Waiting for support in the core, which is missing as of
// 2.0.17 (framework-arduinoespressif32 4.20017); core 3.x replaces
// WiFiServer with a dual-stack NetworkServer.
//
// A global address would be wanted too.  enableIpV6() brings up a link-local
// one, reachable only from the same segment and only with a zone index in
// the URL (http://[fe80::...%2528]/), so it is of little use as a service
// address even once something is listening.
//
// Until both of those are true, an address that cannot be connected to is
// just something else on the screen to be puzzled by, so none of it is
// compiled in.  Setting this to 1 brings the lot back at once.
#ifndef IPV6
#define IPV6 0
#endif

// DERIVED ------------------------------------------------------------------
// Must come last: these are computed from the switches above, so every
// #ifndef default has to have been applied by the time they are evaluated.

// Anything that can change the device at runtime -- the operations layer in
// state.h, the settings it persists, and the state both of them touch -- is
// needed by the serial console and by the REST API alike.  Neither switch
// implies the other, so the shared machinery keys off this instead of either.
#if COMMANDS || NETWORK
#define CONTROLLABLE 1
#else
#define CONTROLLABLE 0
#endif

#endif // CONFIG_H
