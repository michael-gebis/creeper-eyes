// Controller identification probe -- is this panel an SSD1327?
//
//   pio run -e probe1327 -t upload -t monitor
//
// Waveshare's "1.5inch OLED Module" (SSD1327, 16-level grayscale) and
// "1.5inch RGB OLED Module" (SSD1351, 65K colour) have identical 7-pin
// headers, so a wiring check cannot tell them apart. This sends a raw
// SSD1327 init sequence -- no library -- and paints the panel.
//
//   Panel responds (black / white / grey bars)  -> it IS an SSD1327.
//   Panel stays static                          -> it is not; look elsewhere.

#include <Arduino.h>
#include <SPI.h>

#define SCLK_PIN 5
#define MISO_PIN 19
#define MOSI_PIN 18

#define DISPLAY_DC 33
#define DISPLAY_RESET 27
#define CS_LEFT 15
#define CS_RIGHT 4

#define PROBE_HZ 2000000

// SSD1327 is 4 bits per pixel: two pixels share one byte, so a 128-pixel
// row is 64 bytes and the column address range is 0..63.
#define GRAM_COLS 64
#define GRAM_ROWS 128

static SPISettings spiCfg(PROBE_HZ, MSBFIRST, SPI_MODE0);
static int8_t activeCS = -1;

static void select(int8_t cs) {
  activeCS = cs;
  digitalWrite(cs, LOW);
}

static void deselect() {
  if (activeCS >= 0)
    digitalWrite(activeCS, HIGH);
  activeCS = -1;
}

static void cmd(uint8_t c) {
  digitalWrite(DISPLAY_DC, LOW);
  SPI.transfer(c);
  digitalWrite(DISPLAY_DC, HIGH);
}

static void cmd1(uint8_t c, uint8_t arg) {
  digitalWrite(DISPLAY_DC, LOW);
  SPI.transfer(c);
  SPI.transfer(arg); // SSD1327 takes command arguments with DC still low
  digitalWrite(DISPLAY_DC, HIGH);
}

// Init sequence per the SSD1327 datasheet, matching Waveshare's own demo.
static void initSSD1327() {
  cmd(0xAE);        // display off
  cmd1(0x15, 0x00); // column address start ... (end sent below)
  SPI.transfer(0x00);
  digitalWrite(DISPLAY_DC, LOW);
  SPI.transfer(0x3F); // column end = 63
  digitalWrite(DISPLAY_DC, HIGH);

  digitalWrite(DISPLAY_DC, LOW);
  SPI.transfer(0x75); // row address
  SPI.transfer(0x00);
  SPI.transfer(0x7F); // row end = 127
  digitalWrite(DISPLAY_DC, HIGH);

  cmd1(0x81, 0x80); // contrast
  cmd1(0xA0, 0x51); // remap: horizontal, nibble order
  cmd1(0xA1, 0x00); // display start line
  cmd1(0xA2, 0x00); // display offset
  cmd(0xA4);        // normal display
  cmd1(0xA8, 0x7F); // multiplex ratio 128
  cmd1(0xB1, 0xF1); // phase length
  cmd1(0xB3, 0x00); // front clock divider
  cmd1(0xAB, 0x01); // function select A: enable internal VDD
  cmd1(0xB6, 0x0F); // second precharge period
  cmd1(0xBE, 0x0F); // VCOMH
  cmd1(0xBC, 0x08); // precharge voltage
  cmd1(0xD5, 0x62); // function select B
  cmd1(0xFD, 0x12); // command unlock
  cmd(0xAF);        // display on
}

static void setWindow() {
  digitalWrite(DISPLAY_DC, LOW);
  SPI.transfer(0x15);
  SPI.transfer(0x00);
  SPI.transfer(GRAM_COLS - 1);
  SPI.transfer(0x75);
  SPI.transfer(0x00);
  SPI.transfer(GRAM_ROWS - 1);
  digitalWrite(DISPLAY_DC, HIGH);
}

// level 0..15, packed into both nibbles
static void fillGray(uint8_t level) {
  uint8_t b = (uint8_t)((level << 4) | level);
  setWindow();
  for (uint16_t i = 0; i < GRAM_COLS * GRAM_ROWS; i++)
    SPI.transfer(b);
}

// vertical bars stepping through all 16 grey levels
static void grayRamp() {
  setWindow();
  for (uint16_t row = 0; row < GRAM_ROWS; row++) {
    for (uint16_t col = 0; col < GRAM_COLS; col++) {
      uint8_t hi = (uint8_t)((col * 2) / 8);
      uint8_t lo = (uint8_t)((col * 2 + 1) / 8);
      SPI.transfer((uint8_t)((hi << 4) | (lo & 0x0F)));
    }
  }
}

static void paint(int8_t cs, const char *name) {
  SPI.beginTransaction(spiCfg);
  select(cs);

  Serial.printf("  %-5s black\n", name);
  fillGray(0x0);
  delay(900);
  Serial.printf("  %-5s white\n", name);
  fillGray(0xF);
  delay(900);
  Serial.printf("  %-5s grey ramp\n", name);
  grayRamp();
  delay(1600);
  fillGray(0x0);

  deselect();
  SPI.endTransaction();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n[probe] raw SSD1327 init @ %d Hz\n", PROBE_HZ);
  Serial.println("[probe] responds => SSD1327 grayscale panel");
  Serial.println("[probe] static   => not an SSD1327\n");

  pinMode(DISPLAY_DC, OUTPUT);
  digitalWrite(DISPLAY_DC, HIGH);
  pinMode(CS_LEFT, OUTPUT);
  digitalWrite(CS_LEFT, HIGH);
  pinMode(CS_RIGHT, OUTPUT);
  digitalWrite(CS_RIGHT, HIGH);

  SPI.begin(SCLK_PIN, MISO_PIN, MOSI_PIN);

  pinMode(DISPLAY_RESET, OUTPUT);
  digitalWrite(DISPLAY_RESET, HIGH);
  delay(20);
  digitalWrite(DISPLAY_RESET, LOW);
  delay(20);
  digitalWrite(DISPLAY_RESET, HIGH);
  delay(200);

  // Both panels share DC, CLK and DIN, so init each with only its own
  // chip select asserted.
  SPI.beginTransaction(spiCfg);
  select(CS_LEFT);
  initSSD1327();
  deselect();
  SPI.endTransaction();
  Serial.println("[probe] left  init sent");

  SPI.beginTransaction(spiCfg);
  select(CS_RIGHT);
  initSSD1327();
  deselect();
  SPI.endTransaction();
  Serial.println("[probe] right init sent\n");
}

void loop() {
  Serial.println("[probe] LEFT panel");
  paint(CS_LEFT, "left");
  Serial.println("[probe] RIGHT panel");
  paint(CS_RIGHT, "right");
  Serial.println();
}
