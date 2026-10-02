// The panels, for the modules that are not the renderer: text cards, power,
// brightness, waiting out a frame in flight, and which panel is where.
//
// The implementations stay in main.cpp with the rest of the panel handling;
// this only publishes the handful of entry points other modules need, so
// they do not have to know how a panel is written to.

#ifndef DISPLAY_H
#define DISPLAY_H

#include <Adafruit_GFX.h>
#include <stdint.h>

// Panel geometry.  main.cpp static_asserts these against the eye
// artwork's SCREEN_WIDTH/SCREEN_HEIGHT, so the two cannot drift.
#define PANEL_W 128
#define PANEL_H 128

// How many panels are fitted.  Wraps PANEL_COUNT, which is derived from the
// panel array in main.cpp and so cannot be shared directly.
uint8_t displayCount(void);

// Draw a string centred horizontally at the given row, in the default font
// scaled by `size`.
void drawCenteredText(GFXcanvas1 &c, const char *str, uint8_t size, int16_t y);

// Send a 1-bit canvas to one panel, in whichever format that panel wants.
void pushCanvas(uint8_t e, GFXcanvas1 &canvas);

// Four centred lines on every panel.  For moments when the eyes are not
// running and the head would otherwise sit there dark and unexplained.
void showMessage(const char *l1, const char *l2, const char *l3,
                 const char *l4);

// The same, on one panel only.  `which` of -1 means all of them.  The setup
// portal uses this to caption a QR code on the other eye without wiping it.
void showMessageOn(int8_t which, const char *l1, const char *l2,
                   const char *l3, const char *l4);

// Waits until no eye frame is queued or on the wire -- see OVERLAP_SEND.
// For the few things that must not interleave with one: a card, which a
// frame queued before it would otherwise paint over, and a swap or flip,
// which change what a frame in flight is using.  Returns at once when
// nothing is being sent, and before the sender exists.
void displayQuiesce(void);

// Panels lit or dark, via the controller's own command rather than by
// drawing black.  GDDRAM survives, so waking costs one command and no
// redraw.  Both are no-ops when already in the requested state, so callers
// may ask freely.
void displaySetPower(bool on);
bool displayIsOn(void);

// One panel's light output, as a fraction of the most its controller can
// drive: 1.0 is the controller's maximum, and anything above 0 is lit, if
// faintly -- off is displaySetPower(false).  Through the panel's own current
// controls; see dimmer.h, which is the only caller, for why.
void displaySetLuminance(uint8_t e, float fraction);

// The fraction the panels are initialised to -- below 1.0, since neither
// controller's setup runs it at full current.  What "unchanged" means.
float displayInitialLuminance(void);

// Which chip-select position eye `e` is on at the moment -- 0 for
// SELECT_L_PIN, 1 for SELECT_R_PIN.  For settings that belong to the panel
// rather than to the eye, which a swap moves between them.
uint8_t displayPosition(uint8_t e);

#endif // DISPLAY_H
