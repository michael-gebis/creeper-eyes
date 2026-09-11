// DS3231 driver.  See rtc.h.
//
// Written out rather than pulling in RTClib, for the same reason ssd1327.h
// is written out: the part of the chip this project uses is seven registers
// of packed BCD, and a hand-rolled version is smaller, has no dependency to
// track, and puts the one subtle bit -- the oscillator-stop flag -- in plain
// sight instead of behind an abstraction.

#include "rtc.h"

#if RTC

#include "timekeeping.h"
#include <Arduino.h>
#include <Wire.h>

// Timekeeping registers, 0x00 upwards, all BCD.
#define REG_SECONDS 0x00
#define REG_STATUS 0x0F
#define REG_TEMP 0x11

// Status register bit 7: set by the chip whenever the oscillator has stopped
// since it was last cleared -- a flat battery, or a chip fresh from the
// factory.  It is the only honest way to ask "is this a real time?", because
// the registers always hold *something*.
#define STATUS_OSF 0x80

static bool present = false;
static bool valid = false;

// timegm() is a GNU extension that newlib does not expose, and mktime() is
// the wrong tool -- it would read these fields as local time and shift them
// by the offset.  Howard Hinnant's days-from-civil, which is exact for any
// date in range and needs no library or timezone at all.
static time_t utcFromTm(const struct tm &t) {
  int y = t.tm_year + 1900;
  const unsigned m = (unsigned)t.tm_mon + 1;
  const unsigned d = (unsigned)t.tm_mday;
  y -= m <= 2;                                  // March-based year
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);              // 0..399
  const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  const long long days = (long long)era * 146097LL + (long long)doe - 719468LL;
  return (time_t)(days * 86400LL + t.tm_hour * 3600LL + t.tm_min * 60LL +
                  t.tm_sec);
}

static uint8_t fromBcd(uint8_t v) { return (uint8_t)((v >> 4) * 10 + (v & 0x0F)); }
static uint8_t toBcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

// One register read.  Returns false rather than a sentinel, because every
// value in these registers is a legitimate reading.
static bool readRegs(uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission() != 0)
    return false;
  if (Wire.requestFrom((int)RTC_ADDR, (int)n) != n)
    return false;
  for (uint8_t i = 0; i < n; i++)
    buf[i] = (uint8_t)Wire.read();
  return true;
}

static bool writeRegs(uint8_t reg, const uint8_t *buf, uint8_t n) {
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(reg);
  for (uint8_t i = 0; i < n; i++)
    Wire.write(buf[i]);
  return Wire.endTransmission() == 0;
}

bool rtcPresent(void) { return present; }
bool rtcValid(void) { return present && valid; }

bool rtcRead(time_t &utc) {
  uint8_t r[7];
  if (!present || !readRegs(REG_SECONDS, r, sizeof(r)))
    return false;

  struct tm t;
  memset(&t, 0, sizeof(t));
  t.tm_sec = fromBcd(r[0] & 0x7F);
  t.tm_min = fromBcd(r[1] & 0x7F);
  // Bit 6 selects 12-hour mode.  This driver always writes 24-hour, but a
  // chip that arrived set by something else has to be read on its own terms.
  if (r[2] & 0x40) {
    uint8_t h = fromBcd(r[2] & 0x1F) % 12;
    t.tm_hour = (r[2] & 0x20) ? h + 12 : h;
  } else {
    t.tm_hour = fromBcd(r[2] & 0x3F);
  }
  t.tm_mday = fromBcd(r[4] & 0x3F);
  t.tm_mon = fromBcd(r[5] & 0x1F) - 1;         // tm months are 0-based
  t.tm_year = fromBcd(r[6]) + 100;             // tm years are from 1900
  if (r[5] & 0x80)
    t.tm_year += 100;                          // century bit
  t.tm_isdst = 0;

  // The registers hold UTC, so this must not go through mktime(), which
  // would read them as local time and shift them by the offset.
  utc = utcFromTm(t);
  return utc > 0;
}

bool rtcWrite(time_t utc) {
  if (!present)
    return false;
  struct tm t;
  gmtime_r(&utc, &t);

  uint8_t r[7];
  r[0] = toBcd((uint8_t)t.tm_sec);
  r[1] = toBcd((uint8_t)t.tm_min);
  r[2] = toBcd((uint8_t)t.tm_hour);            // bit 6 clear = 24-hour
  r[3] = (uint8_t)(t.tm_wday + 1);             // 1-7, unused here but valid
  r[4] = toBcd((uint8_t)t.tm_mday);
  r[5] = toBcd((uint8_t)(t.tm_mon + 1));
  int year = t.tm_year - 100;                  // 2000..2099
  if (year >= 100) {
    year -= 100;
    r[5] |= 0x80;                              // century
  }
  r[6] = toBcd((uint8_t)year);
  if (!writeRegs(REG_SECONDS, r, sizeof(r)))
    return false;

  // Clearing the stop flag is what makes the write mean "this is now a time
  // you can trust".  Read-modify-write: the other bits control the alarms
  // and the 32 kHz output, and are not ours to flatten.
  uint8_t st;
  if (readRegs(REG_STATUS, &st, 1)) {
    st &= (uint8_t)~STATUS_OSF;
    writeRegs(REG_STATUS, &st, 1);
  }
  valid = true;
  return true;
}

bool rtcWriteNow(void) {
  if (!timeSynced)
    return false;
  return rtcWrite(time(NULL));
}

bool rtcTemperature(float &celsius) {
  uint8_t r[2];
  if (!present || !readRegs(REG_TEMP, r, 2))
    return false;
  // 10-bit two's complement: whole degrees in the first byte, quarters in
  // the top two bits of the second.
  celsius = (float)(int8_t)r[0] + ((r[1] >> 6) * 0.25f);
  return true;
}

void rtcBegin(void) {
  Wire.begin(RTC_SDA_PIN, RTC_SCL_PIN);
  Wire.setTimeOut(50); // never let a shorted bus stall the render loop

  Wire.beginTransmission(RTC_ADDR);
  present = (Wire.endTransmission() == 0);
  if (!present) {
    DEBUG_PRINTF("[rtc] no DS3231 at 0x%02X on SDA=%d SCL=%d; carrying on"
                 "\n", RTC_ADDR, RTC_SDA_PIN, RTC_SCL_PIN);
    return;
  }

  uint8_t st = 0;
  valid = readRegs(REG_STATUS, &st, 1) && !(st & STATUS_OSF);
  if (!valid) {
    DEBUG_PRINTF("[rtc] found, but its oscillator has stopped -- set the "
                 "time with `clock set` and it will be kept" "\n");
    return;
  }

  time_t utc;
  if (!rtcRead(utc)) {
    DEBUG_PRINTF("[rtc] found, but could not be read" "\n");
    return;
  }
  timeAccept(utc, TIME_RTC);

  struct tm t;
  if (timeLocal(t))
    DEBUG_PRINTF("[rtc] time restored: %04d-%02d-%02d %02d:%02d:%02d %s" "\n",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
                 t.tm_min, t.tm_sec, tzString);
}

void rtcReport(Print &out) {
  out.printf("rtc addr=0x%02X sda=%d scl=%d %s" "\n", RTC_ADDR, RTC_SDA_PIN,
             RTC_SCL_PIN, present ? "present" : "not found");
  if (!present)
    return;
  out.printf("  battery %s" "\n",
             valid ? "held; the time is good"
                   : "lost or never set; the time is not to be believed");
  time_t utc;
  if (rtcRead(utc)) {
    struct tm g;
    gmtime_r(&utc, &g);
    out.printf("  chip    %04d-%02d-%02d %02d:%02d:%02d UTC" "\n",
               g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min,
               g.tm_sec);
  }
  float c;
  if (rtcTemperature(c))
    out.printf("  temp    %.2f C" "\n", c);
}

#endif // RTC
