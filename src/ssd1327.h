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

class SSD1327 {
public:
  SSD1327(int8_t csPin, int8_t dcPin) : _cs(csPin), _dc(dcPin) {}

  // Park chip select high.  Call for every panel before initialising any of
  // them, so a shared bus never reaches a panel that isn't listening.
  // Chip select can be reassigned after construction so a miswired pair
  // of panels can be swapped in software.  See the `swap` console command.
  void setCS(int8_t pin) { _cs = pin; }

  void parkCS() const {
    pinMode(_cs, OUTPUT);
    digitalWrite(_cs, HIGH);
  }

  // Pulse the reset line shared by every panel.  Static because it must
  // happen exactly once, before any panel is initialised -- resetting after
  // a panel is up would wipe it.
  static void sharedReset(int8_t rstPin) {
    pinMode(rstPin, OUTPUT);
    digitalWrite(rstPin, HIGH);
    delay(20);
    digitalWrite(rstPin, LOW);
    delay(20);
    digitalWrite(rstPin, HIGH);
    delay(200);
  }

  void begin(SPISettings cfg) {
    pinMode(_dc, OUTPUT);
    digitalWrite(_dc, HIGH);

    SPI.beginTransaction(cfg);
    digitalWrite(_cs, LOW);

    cmd(0xAE);        // display off
    cmdN(0x15, 0x00, SSD1327_COLS - 1);  // column address range
    cmdN(0x75, 0x00, SSD1327_HEIGHT - 1); // row address range
    cmd1(0x81, 0x80); // contrast
    cmd1(0xA0, 0x51); // remap: horizontal increment, nibble order
    cmd1(0xA1, 0x00); // display start line
    cmd1(0xA2, 0x00); // display offset
    cmd(0xA4);        // normal (not all-on / all-off / inverse)
    cmd1(0xA8, 0x7F); // multiplex ratio = 128
    cmd1(0xB1, 0xF1); // phase length
    cmd1(0xB3, 0x00); // front clock divider
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
// it badly.  Three multiplies per pixel is nothing on a 240 MHz core.
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
