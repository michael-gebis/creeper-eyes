// QR legibility test -- not part of the eyes firmware.
//
//   pio run -e qrtest -t upload -t monitor          (SSD1327 grey)
//   pio run -e qrtest_rgb -t upload -t monitor      (SSD1351 colour)
//
// The question this answers is not "can the board draw a QR code" -- it can,
// trivially.  It is "can a phone read a QR code off a 128x128 panel sunk into
// an eye socket", and that depends entirely on how many modules are crammed
// into those 128 pixels.
//
// So every code here is a payload the real feature would actually use, at the
// density it would actually have.  A version-1 QR would scan from across the
// room and prove nothing.
//
// THIS BUILD HAS NO NETWORKING.  Flash it over the air and the board becomes
// unreachable until somebody plugs in a USB cable -- there is no web server
// left to send the next update to.  Flash it over USB, which you want anyway,
// since the measurements come back over serial.
//
// Three phases, ten seconds each, narrated over serial.  Every one of them
// ends up at three pixels per module, because 128 divided by anything in this
// range is three; what changes is how much of the panel gets used:
//
//   1. WiFi join, ECC M -- version 3, 29x29, 111 of 128 px.  The candidate.
//   2. WiFi join, ECC Q -- version 4, 33x33, 123 of 128 px.  Twice the error
//      correction of M, and nearly the whole panel.  Worth trying because a
//      code photographed at an angle, in an eye socket, is exactly the case
//      error correction exists for.
//   3. The address, http://frank.local/ -- version 2, 25x25, 99 of 128 px.
//      The sparsest of the three.  If this one fails, the idea is dead.
//
// ECC L is deliberately absent: at 41 bytes it produces the same version 3 as
// ECC M, so it would be the same picture with less redundancy.
//
// Both panels show the same code at once, so whichever eye is easier to point
// a camera into is the one you use.

#include <Arduino.h>
#include <SPI.h>
#include <qrcode.h>

#include <Adafruit_GFX.h>

#define SCLK_PIN 5
#define MISO_PIN 19
#define MOSI_PIN 18
#define DISPLAY_DC 33
#define DISPLAY_RESET 27
#define CS_LEFT 15
#define CS_RIGHT 4

#define PANEL_W 128
#define PANEL_H 128

// The colour environment does not define it at all, and the runtime report
// below reads it as a value rather than testing it with #if.
#ifndef USE_SSD1327
#define USE_SSD1327 0
#endif

// The AP name the real portal uses, so the payload length is honest.
#define AP_NAME "frank-setup"

#if USE_SSD1327
#include "../ssd1327.h"
// main.cpp is not in this build, so its default is repeated rather than
// shared -- the eyes run the grey panel at this rate too.
#define SSD1327_SPI_HZ 8000000
static SPISettings panelSPI(SSD1327_SPI_HZ, MSBFIRST, SPI_MODE0);
static SSD1327 leftEye(CS_LEFT, DISPLAY_DC);
static SSD1327 rightEye(CS_RIGHT, DISPLAY_DC);
#else
#include <Adafruit_SSD1351.h>
static Adafruit_SSD1351 leftEye(PANEL_W, PANEL_H, &SPI, CS_LEFT, DISPLAY_DC, -1);
static Adafruit_SSD1351 rightEye(PANEL_W, PANEL_H, &SPI, CS_RIGHT, DISPLAY_DC, -1);
#endif

static GFXcanvas1 canvas(PANEL_W, PANEL_H);

// Room for anything up to version 6, which is far more than these payloads
// need; sizing for the largest lets the fitting loop below try small first.
#define MAX_VERSION 6
// qrcode_getBufferSize() is a function, so it cannot size a static array.
// This is the same arithmetic: a version-v code is (4v+17) modules square,
// one bit each.  Checked against bb_getGridSizeBytes() in the library.
#define QR_MODULES(v) (4 * (v) + 17)
#define QR_BUFFER_BYTES(v) (((QR_MODULES(v) * QR_MODULES(v)) + 7) / 8)
static uint8_t qrBuffer[QR_BUFFER_BYTES(MAX_VERSION)];
static QRCode qrcode;

// Both panels share a reset line, so the library must not drive it: resetting
// inside the second begin() would wipe the first.  Done once, here.
static void sharedReset(void) {
  pinMode(DISPLAY_RESET, OUTPUT);
  digitalWrite(DISPLAY_RESET, HIGH);
  delay(20);
  digitalWrite(DISPLAY_RESET, LOW);
  delay(20);
  digitalWrite(DISPLAY_RESET, HIGH);
  delay(20);
}

// The smallest version that holds the payload.  The library will not choose
// for us, and the version is the whole point of this test -- it decides how
// many modules land in 128 pixels.
static bool fit(const char *text, uint8_t ecc, uint8_t &versionOut) {
  for (uint8_t v = 1; v <= MAX_VERSION; v++) {
    if (qrcode_initText(&qrcode, qrBuffer, v, ecc, text) == 0) {
      versionOut = v;
      return true;
    }
  }
  return false;
}

static void pushBoth(void) {
#if USE_SSD1327
  // 1 bit in, 4 bits out, two pixels to a byte -- the same packing the eyes
  // use.  White modules go to full brightness so the contrast is maximal.
  static uint8_t packed[SSD1327_FRAME_BYTES];
  uint16_t o = 0;
  for (int16_t y = 0; y < PANEL_H; y++)
    for (int16_t x = 0; x < PANEL_W; x += 2, o++)
      packed[o] = (uint8_t)((canvas.getPixel(x, y) ? 0xF0 : 0x00) |
                            (canvas.getPixel(x + 1, y) ? 0x0F : 0x00));
  SPI.beginTransaction(panelSPI);
  leftEye.pushFrame(packed);
  rightEye.pushFrame(packed);
  SPI.endTransaction();
#else
  leftEye.drawBitmap(0, 0, canvas.getBuffer(), PANEL_W, PANEL_H, 0xFFFF, 0x0000);
  rightEye.drawBitmap(0, 0, canvas.getBuffer(), PANEL_W, PANEL_H, 0xFFFF,
                      0x0000);
#endif
}

// Draw the code currently in `qrcode`, as large as it will go.
//
// The quiet zone is not decoration: the spec wants four clear modules on every
// side and readers genuinely fail without it.  It is included in the
// arithmetic rather than added afterwards, so the scale is honest.
static uint8_t drawQR(void) {
  const uint8_t quiet = 4;
  const uint16_t span = qrcode.size + 2 * quiet;
  const uint8_t scale = (uint8_t)(PANEL_W / span); // whole pixels only
  const uint16_t drawn = span * scale;
  const int16_t off = (int16_t)((PANEL_W - drawn) / 2);

  // White ground: on an OLED that is the lit state, and a QR wants a light
  // background with dark modules.
  canvas.fillScreen(1);
  for (uint8_t y = 0; y < qrcode.size; y++)
    for (uint8_t x = 0; x < qrcode.size; x++)
      if (qrcode_getModule(&qrcode, x, y))
        canvas.fillRect(off + (quiet + x) * scale, off + (quiet + y) * scale,
                        scale, scale, 0);
  pushBoth();
  return scale;
}

static void phase(const char *what, const char *text, uint8_t ecc,
                  const char *eccName) {
  uint8_t version = 0;
  if (!fit(text, ecc, version)) {
    Serial.printf("\n%s: DOES NOT FIT in version %d or below\n", what,
                  MAX_VERSION);
    return;
  }
  uint8_t scale = drawQR();
  const uint8_t quiet = 4;
  Serial.printf("\n%s\n", what);
  Serial.printf("  payload   %s\n", text);
  Serial.printf("  bytes     %u\n", (unsigned)strlen(text));
  Serial.printf("  ecc       %s\n", eccName);
  Serial.printf("  version   %u  (%ux%u modules)\n", (unsigned)version,
                (unsigned)qrcode.size, (unsigned)qrcode.size);
  Serial.printf("  with quiet zone  %u modules across\n",
                (unsigned)(qrcode.size + 2 * quiet));
  Serial.printf("  scale     %u px per module -> %u of %u px used\n",
                (unsigned)scale,
                (unsigned)((qrcode.size + 2 * quiet) * scale),
                (unsigned)PANEL_W);
  if (scale < 3)
    Serial.printf("  NOTE: under 3 px per module is optimistic for a phone\n");
  Serial.printf("  scan it now -- ten seconds\n");
}

void setup(void) {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n\nQR legibility test on a %dx%d panel\n", PANEL_W, PANEL_H);
  Serial.printf("panel: %s\n", USE_SSD1327 ? "SSD1327 grey" : "SSD1351 colour");
  Serial.printf("\nEvery code below is a payload the real feature would use,\n"
                "at the density it would really have.  Scan each one and note\n"
                "which succeed, from the distance and angle you would in a\n"
                "finished head.\n");

  SPI.begin(SCLK_PIN, MISO_PIN, MOSI_PIN);
  pinMode(CS_LEFT, OUTPUT);
  pinMode(CS_RIGHT, OUTPUT);
  digitalWrite(CS_LEFT, HIGH);
  digitalWrite(CS_RIGHT, HIGH);
  sharedReset();

#if USE_SSD1327
  leftEye.begin(panelSPI);
  rightEye.begin(panelSPI);
#else
  leftEye.begin();
  rightEye.begin();
#endif
}

void loop(void) {
  // The password the real feature would derive from the MAC: six bytes as
  // twelve hex characters, so the payload length here is exactly what it
  // would be in the field rather than a guess.
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char wifiPayload[96];
  snprintf(wifiPayload, sizeof(wifiPayload),
           "WIFI:T:WPA;S:%s;P:%02X%02X%02X%02X%02X%02X;;", AP_NAME, mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5]);

  phase("1. WiFi join, ECC M -- the candidate", wifiPayload, ECC_MEDIUM,
        "M (15% recovery)");
  delay(10000);

  phase("2. WiFi join, ECC Q -- denser, twice the error correction",
        wifiPayload, ECC_QUARTILE, "Q (25% recovery)");
  delay(10000);

  phase("3. Address, ECC M -- the easy one", "http://frank.local/", ECC_MEDIUM,
        "M (15% recovery)");
  delay(10000);
}
