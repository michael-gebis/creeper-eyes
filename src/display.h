// Text rendering shared by the startup splash and the network address cards.
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

// How many panels are fitted.  Wraps NUM_EYES, which is derived from an
// anonymous struct and so cannot be shared directly.
uint8_t displayCount(void);

// Draw a string centred horizontally at the given row, in the default font
// scaled by `size`.
void splashCenter(GFXcanvas1 &c, const char *str, uint8_t size, int16_t y);

// Send a 1-bit canvas to one panel, in whichever format that panel wants.
void pushCanvas(uint8_t e, GFXcanvas1 &canvas);

// Four centred lines on every panel.  For moments when the eyes are not
// running and the head would otherwise sit there dark and unexplained.
void showMessage(const char *l1, const char *l2, const char *l3,
                 const char *l4);

#endif // DISPLAY_H
