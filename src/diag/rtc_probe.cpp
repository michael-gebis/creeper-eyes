// Bring up a DS3231 without any of the rest of the firmware in the way.
//
//   pio run -e rtc_probe -t upload -t monitor
//
// The same idea as ssd1327_probe.cpp: when a new part will not talk, the
// question is always "is it the wiring or is it my code", and the only way to
// answer it is to take the code out of the picture.  This scans the bus,
// reports every address that answers, and then reads the chip raw.
//
// What you should see, with a module wired per docs/WIRING_RTC.md:
//
//   scan: found 0x57  (AT24C32 EEPROM)
//   scan: found 0x68  (DS3231)
//   status 0x00  oscillator has run continuously
//   raw 00..06: 12 34 09 03 10 09 26
//   -> 2026-09-10 09:34:12 UTC   temp 24.75 C
//
// A bus with nothing on it reports no addresses at all: check 3V3, GND, and
// that SDA and SCL are not swapped.  Only 0x57 answering means the DS3231
// half of the board is not being powered or is faulty.

#include <Arduino.h>
#include <Wire.h>

#define SDA_PIN 21
#define SCL_PIN 22
#define DS3231 0x68

static uint8_t fromBcd(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }

static bool readRegs(uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(DS3231);
  Wire.write(reg);
  if (Wire.endTransmission() != 0)
    return false;
  if (Wire.requestFrom((int)DS3231, (int)n) != n)
    return false;
  for (uint8_t i = 0; i < n; i++)
    buf[i] = (uint8_t)Wire.read();
  return true;
}

void setup(void) {
  Serial.begin(115200);
  delay(400);
  Serial.println();
  Serial.printf("[rtc probe] SDA=%d SCL=%d\n", SDA_PIN, SCL_PIN);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setTimeOut(50);
}

void loop(void) {
  // A full scan every time, because the interesting failure is intermittent
  // wiring, and a single boot-time scan would miss it.
  uint8_t found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      found++;
      const char *what = a == DS3231  ? "  (DS3231)"
                         : a == 0x57  ? "  (AT24C32 EEPROM)"
                                      : "";
      Serial.printf("scan: found 0x%02X%s\n", a, what);
    }
  }
  if (!found)
    Serial.println("scan: nothing on the bus -- check 3V3, GND, SDA and SCL");

  uint8_t st;
  if (readRegs(0x0F, &st, 1))
    Serial.printf("status 0x%02X  %s\n", st,
                  (st & 0x80) ? "OSCILLATOR HAS STOPPED -- time not valid"
                              : "oscillator has run continuously");

  uint8_t r[7];
  if (readRegs(0x00, r, sizeof(r))) {
    Serial.printf("raw 00..06:");
    for (uint8_t i = 0; i < sizeof(r); i++)
      Serial.printf(" %02X", r[i]);
    Serial.println();
    Serial.printf("-> 20%02u-%02u-%02u %02u:%02u:%02u UTC",
                  fromBcd(r[6]), fromBcd(r[5] & 0x1F), fromBcd(r[4] & 0x3F),
                  fromBcd(r[2] & 0x3F), fromBcd(r[1] & 0x7F),
                  fromBcd(r[0] & 0x7F));
    uint8_t t[2];
    if (readRegs(0x11, t, 2))
      Serial.printf("   temp %.2f C", (float)(int8_t)t[0] + (t[1] >> 6) * 0.25f);
    Serial.println();
  }

  Serial.println();
  delay(3000);
}
