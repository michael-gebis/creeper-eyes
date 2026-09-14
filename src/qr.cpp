// See qr.h.

#include "qr.h"

#if QR_CODES

#include "display.h"
#include <Adafruit_GFX.h>
#include <qrcode.h>
#include <string.h>

// Version 3 is the ceiling, and it is a measured one rather than a guess.
//
// Every code this project draws lands at three pixels per module -- 128
// divided by anything in this range is three -- so module size is not the
// variable.  What changes is how many modules there are, and version 4 fills
// 123 of the panel's 128 pixels with nothing to spare.  A phone reads version
// 3 reliably and struggles to lock onto version 4, so this refuses to draw
// one.  docs/QR.md has the measurements.
#define QR_MAX_VERSION 3
#define QR_ECC ECC_MEDIUM

// Byte-mode capacity from ISO/IEC 18004 table 7, by version, at ECC M.
//
// Checked here because the library does not check: qrcode.c carries
// "@TODO: Return error if data is too big" above its init function and means
// it, returning success for any length.  Trusting it produced well-formed
// grids of nothing, labelled with a version that was not what had been drawn.
static const uint8_t QR_CAPACITY_M[QR_MAX_VERSION] = {14, 26, 42};

#define QR_MODULES(v) (4 * (v) + 17)
#define QR_BUFFER_BYTES(v) (((QR_MODULES(v) * QR_MODULES(v)) + 7) / 8)

uint16_t qrCapacity(void) { return QR_CAPACITY_M[QR_MAX_VERSION - 1]; }

bool qrShow(uint8_t eye, const char *text) {
  if (!text || eye >= displayCount())
    return false;

  size_t len = strlen(text);
  uint8_t version = 0;
  for (uint8_t v = 1; v <= QR_MAX_VERSION; v++) {
    if (len <= QR_CAPACITY_M[v - 1]) {
      version = v;
      break;
    }
  }
  if (!version)
    return false; // too long to draw legibly; say so rather than draw mush

  static uint8_t buffer[QR_BUFFER_BYTES(QR_MAX_VERSION)];
  QRCode qrcode;
  if (qrcode_initText(&qrcode, buffer, version, QR_ECC, text) != 0)
    return false;

  // The quiet zone is not decoration: the spec wants four clear modules on
  // every side, and readers genuinely fail without it.  It is part of the
  // scale arithmetic rather than something added afterwards.
  const uint8_t quiet = 4;
  const uint16_t span = qrcode.size + 2 * quiet;
  const uint8_t scale = (uint8_t)(PANEL_W / span);
  if (!scale)
    return false;
  const int16_t off = (int16_t)((PANEL_W - span * scale) / 2);

  // Light ground with dark modules, which is the way round a reader expects.
  // On an OLED that means the quiet zone is the lit part.
  static GFXcanvas1 canvas(PANEL_W, PANEL_H);
  canvas.fillScreen(1);
  for (uint8_t y = 0; y < qrcode.size; y++)
    for (uint8_t x = 0; x < qrcode.size; x++)
      if (qrcode_getModule(&qrcode, x, y))
        canvas.fillRect(off + (quiet + x) * scale, off + (quiet + y) * scale,
                        scale, scale, 0);

  pushCanvas(eye, canvas);
  return true;
}

#endif // QR_CODES
