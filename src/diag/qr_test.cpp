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
// Two codes, narrated over serial.  Both of them
// ends up at three pixels per module, because 128 divided by anything in this
// range is three; what changes is how much of the panel gets used:
//
//   1. WiFi join, ECC M -- version 3, 29x29, 111 of 128 px.  The candidate.
//   2. WiFi join, ECC Q -- version 4, 33x33, 123 of 128 px.  Twice the error
//      correction of M, and nearly the whole panel.  Worth trying because a
//      code photographed at an angle, in an eye socket, is exactly the case
//      error correction exists for.
//   3. The address, http://192.168.123.166/ -- version 2, 25x25, 99 of 128
//      px.  An address rather than frank.local: see ADDRESS_URL below.
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

// An address, not a name.  frank.local needs mDNS, which Windows does not
// have without Bonjour and Android only resolves reliably from 12 onward;
// a dotted quad works anywhere on the subnet.  There is no networking in
// this build to ask, so it is fixed -- but it is the real address, and at
// fifteen characters it is also as long as IPv4 gets, so the payload is the
// worst case rather than a flattering one.
#define ADDRESS_URL "http://192.168.123.166/"

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

// Byte-mode capacity in bytes, versions 1-6 by ECC level, from ISO/IEC 18004
// table 7.  Column order matches the library's ECC_LOW/MEDIUM/QUARTILE/HIGH,
// which are 0-3.
static const uint8_t QR_CAPACITY[MAX_VERSION][4] = {
    {17, 14, 11, 7},     // version 1
    {32, 26, 20, 14},    // version 2
    {53, 42, 32, 24},    // version 3
    {78, 62, 46, 34},    // version 4
    {106, 84, 60, 44},   // version 5
    {134, 106, 74, 58},  // version 6
};

// The smallest version that holds the payload.
//
// The capacity check has to happen here because the library does not do it:
// qrcode.c carries "@TODO: Return error if data is too big" directly above
// qrcode_initBytes(), and true to the comment it returns success for any
// length.  Asking it to encode forty-one bytes as a version-1 code produced a
// perfectly well-formed twenty-one module grid containing nothing readable,
// and reported version 1 while doing it -- which is exactly the kind of test
// result that looks like a scanning problem and is not.
static bool fit(const char *text, uint8_t ecc, uint8_t &versionOut) {
  size_t len = strlen(text);
  for (uint8_t v = 1; v <= MAX_VERSION; v++) {
    if (len > QR_CAPACITY[v - 1][ecc])
      continue;
    if (qrcode_initText(&qrcode, qrBuffer, v, ecc, text) != 0)
      continue;
    versionOut = v;
    return true;
  }
  return false;
}

// The same code as text, two characters per module.
//
// Two reasons.  It lets this test be checked without a panel at all -- scan
// it off the terminal, and if that works while the panel does not, the
// encoding is fine and the problem is optics.  And it makes a malformed code
// obvious to a person: three finder squares in the corners, a timing line
// between them.  The version-1 nonsense above looked wrong at a glance.
static void dumpAscii(void) {
  const uint8_t quiet = 2; // narrower than the spec's 4, to fit a terminal
  for (uint8_t i = 0; i < quiet; i++) {
    for (uint16_t x = 0; x < (uint16_t)(qrcode.size + 2 * quiet); x++)
      Serial.print("  ");
    Serial.println();
  }
  for (uint8_t y = 0; y < qrcode.size; y++) {
    for (uint8_t i = 0; i < quiet; i++)
      Serial.print("  ");
    for (uint8_t x = 0; x < qrcode.size; x++)
      Serial.print(qrcode_getModule(&qrcode, x, y) ? "##" : "  ");
    Serial.println();
  }
  for (uint8_t i = 0; i < quiet; i++) {
    for (uint16_t x = 0; x < (uint16_t)(qrcode.size + 2 * quiet); x++)
      Serial.print("  ");
    Serial.println();
  }
}

// Which panel a draw goes to.  The names are the viewer's, matching the other
// diagnostics: LEFT is CS_LEFT, the panel on your left as you look at the
// head, which is Frank's right eye.
enum Panel { LEFT, RIGHT };

static void pushTo(Panel which) {
#if USE_SSD1327
  // 1 bit in, 4 bits out, two pixels to a byte -- the same packing the eyes
  // use.  Set pixels go to full brightness so the contrast is maximal.
  static uint8_t packed[SSD1327_FRAME_BYTES];
  uint16_t o = 0;
  for (int16_t y = 0; y < PANEL_H; y++)
    for (int16_t x = 0; x < PANEL_W; x += 2, o++)
      packed[o] = (uint8_t)((canvas.getPixel(x, y) ? 0xF0 : 0x00) |
                            (canvas.getPixel(x + 1, y) ? 0x0F : 0x00));
  SPI.beginTransaction(panelSPI);
  if (which == LEFT)
    leftEye.pushFrame(packed);
  else
    rightEye.pushFrame(packed);
  SPI.endTransaction();
#else
  if (which == LEFT)
    leftEye.drawBitmap(0, 0, canvas.getBuffer(), PANEL_W, PANEL_H, 0xFFFF,
                       0x0000);
  else
    rightEye.drawBitmap(0, 0, canvas.getBuffer(), PANEL_W, PANEL_H, 0xFFFF,
                        0x0000);
#endif
}

// A caption for the left eye: three lines, the middle one large.
//
// Dark ground rather than light, unlike the QR.  A second lit panel beside
// the code would drag the camera's exposure down and make the thing it is
// trying to read worse.
static void caption(const char *top, const char *big, const char *bottom) {
  canvas.fillScreen(0);
  canvas.setTextColor(1);

  canvas.setTextSize(2);
  int16_t w = (int16_t)strlen(top) * 12;
  canvas.setCursor((PANEL_W - w) / 2, 18);
  canvas.print(top);

  canvas.setTextSize(4);
  w = (int16_t)strlen(big) * 24;
  canvas.setCursor((PANEL_W - w) / 2, 50);
  canvas.print(big);

  canvas.setTextSize(2);
  w = (int16_t)strlen(bottom) * 12;
  canvas.setCursor((PANEL_W - w) / 2, 96);
  canvas.print(bottom);

  pushTo(LEFT);
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
  pushTo(RIGHT);
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
  {
    char v[6], m[12];
    snprintf(v, sizeof(v), "v%u", (unsigned)version);
    snprintf(m, sizeof(m), "%ux%u", (unsigned)qrcode.size,
             (unsigned)qrcode.size);
    caption(eccName[0] == 'M' ? "ECC M" : eccName[0] == 'Q' ? "ECC Q" : "ECC L",
            v, m);
  }
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
  Serial.printf("  scan it now -- ten seconds.  The same code in text, as a\n"
                "  control: if this scans off the screen and the panel does\n"
                "  not, the encoding is right and the panel is the problem.\n\n");
  dumpAscii();
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

static void buildWifiPayload(char *out, size_t n) {
  // The password the real feature would derive from the MAC: six bytes as
  // twelve hex characters, so the payload length here is exactly what it
  // would be in the field rather than a guess.
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(out, n, "WIFI:T:WPA;S:%s;P:%02X%02X%02X%02X%02X%02X;;", AP_NAME,
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void loop(void) {
  char wifiPayload[96];
  buildWifiPayload(wifiPayload, sizeof(wifiPayload));

#if USE_SSD1327
  // 0xF0 measured steady through a camera where 0x00 -- the slowest
  // oscillator, and what begin() used to send -- beat visibly.  begin() sends
  // 0xF0 now, so this only restores it after a reset that did not run it.
  leftEye.setFrontClock(panelSPI, 0xF0);
  rightEye.setFrontClock(panelSPI, 0xF0);
#endif

  Serial.printf("\n\n==== legibility ====\n");
  phase("1. WiFi join, ECC M", wifiPayload, ECC_MEDIUM,
        "M (15% recovery)");
  delay(15000);

  phase("2. Address, ECC M", ADDRESS_URL, ECC_MEDIUM,
        "M (15% recovery)");
  delay(15000);
}
