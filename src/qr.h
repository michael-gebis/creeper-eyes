// QR codes on the panels.
//
// Two of them, answering two different questions.  During setup the head
// shows a code that joins its own access point, so a phone camera replaces
// hunting through a list of networks; once it is on your network it can show
// a code that opens its control page, so nobody has to type an address.
//
// What can actually be read off a 128x128 panel in an eye socket was measured
// rather than assumed -- see docs/QR.md.  The short version is that version 3
// and below scan reliably and version 4 does not, which is the constraint
// every payload here is built to fit.

#ifndef QR_H
#define QR_H

#include "config.h"

#if QR_CODES

#include <stdint.h>

// Render `text` as a QR code filling one panel, and push it.
//
// `eye` indexes the panels the same way `eye[]` in main.cpp does: 0 is
// Frank's right, which is the viewer's left.  Both perspectives are in use
// here -- the CS pin names and the diagnostics are the viewer's, anything
// telling somebody which eye to look at is Frank's -- so callers of this say
// whose side they mean rather than leaving it to be worked out.
//
// Returns false if the payload will not fit in a version this panel can show
// legibly, in which case nothing is drawn: a code too dense to read is worse
// than no code, because it looks like it should work.
bool qrShow(uint8_t eye, const char *text);

// The largest payload qrShow() will accept, in bytes.  Exposed so callers can
// check before building a string rather than after.
uint16_t qrCapacity(void);

#endif // QR_CODES
#endif // QR_H
