// Display bring-up diagnostic -- not part of the eyes firmware.
//
//   pio run -e displaytest -t upload -t monitor
//
// Drives each panel on its own with the simplest possible code: solid fills,
// one display at a time, narrated over serial. If solid colours appear here,
// the wiring, the SPI bus and the init sequence are all good and the fault is
// in the eye rendering. If it is still static, the fault is below that level.

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1351.h>
#include <SPI.h>

#define SCLK_PIN 5
#define MISO_PIN 19
#define MOSI_PIN 18

#define DISPLAY_DC 33
#define DISPLAY_RESET 27
#define CS_LEFT 15
#define CS_RIGHT 4

// Start slow. A breadboard with jumper wires and two panels on one bus is a
// poor transmission line; if 2 MHz is clean and 8 MHz is not, the wiring is
// marginal rather than wrong.
#define DIAG_SPI_HZ 2000000

// rst = -1 on BOTH panels. The library resets inside begin(), and with a
// shared reset line that means initialising the second panel wipes the first.
// We drive the shared reset ourselves, exactly once, below.
Adafruit_SSD1351 leftEye(128, 128, &SPI, CS_LEFT, DISPLAY_DC, -1);
Adafruit_SSD1351 rightEye(128, 128, &SPI, CS_RIGHT, DISPLAY_DC, -1);

static void sharedReset() {
  pinMode(DISPLAY_RESET, OUTPUT);
  digitalWrite(DISPLAY_RESET, HIGH);
  delay(20);
  digitalWrite(DISPLAY_RESET, LOW);
  delay(20);
  digitalWrite(DISPLAY_RESET, HIGH);
  delay(200);
}

static void show(Adafruit_SSD1351 &d, const char *panel, const char *colour,
                 uint16_t rgb) {
  Serial.printf("  %-5s -> %s\n", panel, colour);
  d.fillScreen(rgb);
  delay(1200);
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n[diag] SSD1351 bring-up @ %d Hz\n", DIAG_SPI_HZ);
  Serial.printf("[diag] SCK=%d MOSI=%d DC=%d RST=%d CS_L=%d CS_R=%d\n",
                SCLK_PIN, MOSI_PIN, DISPLAY_DC, DISPLAY_RESET, CS_LEFT,
                CS_RIGHT);

  // Park both chip selects high before anything else, so neither panel
  // listens while the other is being set up.
  pinMode(CS_LEFT, OUTPUT);
  digitalWrite(CS_LEFT, HIGH);
  pinMode(CS_RIGHT, OUTPUT);
  digitalWrite(CS_RIGHT, HIGH);

  SPI.begin(SCLK_PIN, MISO_PIN, MOSI_PIN);

  sharedReset();
  Serial.println("[diag] shared reset pulsed once");

  leftEye.begin(DIAG_SPI_HZ);
  Serial.println("[diag] left  init sent");
  rightEye.begin(DIAG_SPI_HZ);
  Serial.println("[diag] right init sent");

  // Both black: any panel still showing static never took its init.
  leftEye.fillScreen(0x0000);
  rightEye.fillScreen(0x0000);
  Serial.println("[diag] both cleared to black -- static now means no init\n");
}

void loop() {
  Serial.println("[diag] LEFT panel only (right should stay black)");
  show(leftEye, "left", "RED", 0xF800);
  show(leftEye, "left", "GREEN", 0x07E0);
  show(leftEye, "left", "BLUE", 0x001F);
  show(leftEye, "left", "WHITE", 0xFFFF);
  leftEye.fillScreen(0x0000);

  Serial.println("[diag] RIGHT panel only (left should stay black)");
  show(rightEye, "right", "RED", 0xF800);
  show(rightEye, "right", "GREEN", 0x07E0);
  show(rightEye, "right", "BLUE", 0x001F);
  show(rightEye, "right", "WHITE", 0xFFFF);
  rightEye.fillScreen(0x0000);

  Serial.println("[diag] both panels: split red / blue");
  leftEye.fillScreen(0xF800);
  rightEye.fillScreen(0x001F);
  delay(2000);
  leftEye.fillScreen(0x0000);
  rightEye.fillScreen(0x0000);
  Serial.println();
}
