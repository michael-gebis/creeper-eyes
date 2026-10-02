// Minimal SSD1327 driver for the twin-eye display setup.
//
// The SSD1327 is a 128x128 controller with 16 grey levels packed two pixels
// to a byte, so a row is 64 bytes rather than the SSD1351's 256.  Both panels
// share DIN, CLK, DC and RST; only chip select distinguishes them.
//
// This is deliberately not GFX-based: the eye renderer produces a whole frame
// at once and streams it, so all that is needed is init, a window, and a
// bulk push.

#ifndef SSD1327_H
#define SSD1327_H

#include <Arduino.h>
#include <SPI.h>

#define SSD1327_WIDTH 128
#define SSD1327_HEIGHT 128

// Two pixels per byte.
#define SSD1327_COLS (SSD1327_WIDTH / 2)
#define SSD1327_FRAME_BYTES (SSD1327_COLS * SSD1327_HEIGHT)

// Register 0xA0, "remap".  Bit 0 reverses the column order, bit 1 swaps the
// two nibbles within a byte, bit 4 reverses the COM scan and bit 6 enables
// the odd/even COM split.  The way up the panel was mounted is settled by
// the first three, and every pixel ever sent goes through them, so a panel
// fitted upside down is put right by changing this one byte rather than by
// touching any frame.
//
// 0x51 is the orientation every driver for this module ships with.  0x42
// undoes both reversals -- and because reversing the column order also
// reverses which nibble is the left-hand pixel, the nibble bit has to flip
// with it.  The pair is the flip0/flip1 sequence u8g2 uses for the same
// controller, and 0x42 is confirmed on this panel: the image comes up
// rotated, not mirrored, with no pixel pairs swapped.  See docs/EYES.md,
// "If a panel is upside down".
#define SSD1327_REMAP_NORMAL 0x51
#define SSD1327_REMAP_FLIPPED 0x42

class SSD1327 {
public:
  SSD1327(int8_t csPin, int8_t dcPin) : _cs(csPin), _dc(dcPin) {}

  // Chip select can be reassigned after construction so a miswired pair
  // of panels can be swapped in software.  See the `swap` console command.
  void setCS(int8_t pin) { _cs = pin; }

  void begin(SPISettings cfg) {
    pinMode(_dc, OUTPUT);
    digitalWrite(_dc, HIGH);

    SPI.beginTransaction(cfg);
    digitalWrite(_cs, LOW);

    cmd(0xAE);        // display off
    cmdN(0x15, 0x00, SSD1327_COLS - 1);  // column address range
    cmdN(0x75, 0x00, SSD1327_HEIGHT - 1); // row address range
    cmd1(0x81, 0x80); // contrast
    cmd1(0xA0, SSD1327_REMAP_NORMAL); // remap: see the defines above
    cmd1(0xA1, 0x00); // display start line
    cmd1(0xA2, 0x00); // display offset
    cmd(0xA4);        // normal (not all-on / all-off / inverse)
    cmd1(0xA8, 0x7F); // multiplex ratio = 128
    cmd1(0xB1, 0xF1); // phase length
    // Fastest oscillator, no division: see setFrontClock().
    cmd1(0xB3, 0xF0); // front clock divider / oscillator frequency
    cmd1(0xAB, 0x01); // function select A: internal VDD regulator
    cmd1(0xB6, 0x0F); // second precharge period
    cmd1(0xBE, 0x0F); // VCOMH
    cmd1(0xBC, 0x08); // precharge voltage
    cmd1(0xD5, 0x62); // function select B
    cmd1(0xFD, 0x12); // command unlock
    cmd(0xAF);        // display on

    digitalWrite(_cs, HIGH);
    SPI.endTransaction();
  }

  // Stream a full packed frame.  Caller owns the SPI transaction so that a
  // frame can be built and pushed without re-arbitrating the bus.
  void pushFrame(const uint8_t *packed) {
    digitalWrite(_cs, LOW);

    digitalWrite(_dc, LOW);
    SPI.transfer(0x15);
    SPI.transfer(0x00);
    SPI.transfer(SSD1327_COLS - 1);
    SPI.transfer(0x75);
    SPI.transfer(0x00);
    SPI.transfer(SSD1327_HEIGHT - 1);
    digitalWrite(_dc, HIGH);

    // writeBytes uses the ESP32's SPI FIFO in bulk rather than byte at a
    // time; the buffer is const in practice but the API is not.
    SPI.writeBytes(const_cast<uint8_t *>(packed), SSD1327_FRAME_BYTES);

    digitalWrite(_cs, HIGH);
  }

  void fill(SPISettings cfg, uint8_t level) {
    uint8_t b = (uint8_t)((level << 4) | (level & 0x0F));
    SPI.beginTransaction(cfg);
    digitalWrite(_cs, LOW);

    digitalWrite(_dc, LOW);
    SPI.transfer(0x15);
    SPI.transfer(0x00);
    SPI.transfer(SSD1327_COLS - 1);
    SPI.transfer(0x75);
    SPI.transfer(0x00);
    SPI.transfer(SSD1327_HEIGHT - 1);
    digitalWrite(_dc, HIGH);

    for (uint16_t i = 0; i < SSD1327_FRAME_BYTES; i++)
      SPI.transfer(b);

    digitalWrite(_cs, HIGH);
    SPI.endTransaction();
  }

  // The right way up, or rotated by 180 degrees, for a panel that was
  // mounted upside down.  A single command; the next frame lands the other
  // way, and so does the one already showing.
  void setFlip(SPISettings cfg, bool flipped) {
    SPI.beginTransaction(cfg);
    digitalWrite(_cs, LOW);
    cmd1(0xA0, flipped ? SSD1327_REMAP_FLIPPED : SSD1327_REMAP_NORMAL);
    digitalWrite(_cs, HIGH);
    SPI.endTransaction();
  }

  // Panel on or off, for sleep mode.  The contents of GDDRAM survive this,
  // so waking does not need a redraw -- which is the whole reason to use the
  // panel's own command rather than pushing a frame of black.
  void setPower(SPISettings cfg, bool on) {
    SPI.beginTransaction(cfg);
    digitalWrite(_cs, LOW);
    cmd(on ? 0xAF : 0xAE);
    digitalWrite(_cs, HIGH);
    SPI.endTransaction();
  }

  // Front clock divider and oscillator frequency, register 0xB3.
  //
  // Low nibble is the DCLK divide ratio, high nibble the oscillator
  // frequency, and together with the phase lengths and the multiplex ratio
  // they set how often the panel scans itself.  begin() uses 0xF0 -- fastest
  // oscillator, no division -- because 0x00, the slowest, scans slowly enough
  // to beat visibly against a camera's rolling shutter.  0xF0 was chosen by
  // sweeping the range with one fixed image on screen; see docs/QR.md.
  void setFrontClock(SPISettings cfg, uint8_t value) {
    SPI.beginTransaction(cfg);
    digitalWrite(_cs, LOW);
    cmd1(0xB3, value);
    digitalWrite(_cs, HIGH);
    SPI.endTransaction();
  }

  // Phase 1 and phase 2 lengths, register 0xB1: the other term in the frame
  // frequency.  begin() uses 0xF1, a long phase 2.
  void setPhaseLength(SPISettings cfg, uint8_t value) {
    SPI.beginTransaction(cfg);
    digitalWrite(_cs, LOW);
    cmd1(0xB1, value);
    digitalWrite(_cs, HIGH);
    SPI.endTransaction();
  }

  // 0 is very dark but not off; 0x80 is what begin() sets.
  void setContrast(SPISettings cfg, uint8_t level) {
    SPI.beginTransaction(cfg);
    digitalWrite(_cs, LOW);
    cmd1(0x81, level);
    digitalWrite(_cs, HIGH);
    SPI.endTransaction();
  }

private:
  int8_t _cs, _dc;

  void cmd(uint8_t c) {
    digitalWrite(_dc, LOW);
    SPI.transfer(c);
    digitalWrite(_dc, HIGH);
  }

  void cmd1(uint8_t c, uint8_t a) {
    digitalWrite(_dc, LOW);
    SPI.transfer(c);
    SPI.transfer(a);
    digitalWrite(_dc, HIGH);
  }

  void cmdN(uint8_t c, uint8_t a, uint8_t b) {
    digitalWrite(_dc, LOW);
    SPI.transfer(c);
    SPI.transfer(a);
    SPI.transfer(b);
    digitalWrite(_dc, HIGH);
  }
};

// RGB565 -> 4-bit grey, Rec.601 luma.
//
// The eye artwork carries a lot of its detail in hue rather than brightness
// (a hazel iris against a warm sclera), so a green-channel shortcut flattens
// it badly.  Three multiplies per pixel is nothing, even at 160 MHz.
static inline uint8_t rgb565ToGray4(uint16_t p) {
  uint8_t r5 = (uint8_t)((p >> 11) & 0x1F);
  uint8_t g6 = (uint8_t)((p >> 5) & 0x3F);
  uint8_t b5 = (uint8_t)(p & 0x1F);

  // Expand to full 8-bit range (replicate high bits into the low ones).
  uint16_t r8 = (uint16_t)((r5 << 3) | (r5 >> 2));
  uint16_t g8 = (uint16_t)((g6 << 2) | (g6 >> 4));
  uint16_t b8 = (uint16_t)((b5 << 3) | (b5 >> 2));

  uint16_t luma = (uint16_t)((77 * r8 + 150 * g8 + 29 * b8) >> 8);
  return (uint8_t)(luma >> 4);
}

#endif // SSD1327_H
