// Battery-backed real-time clock: a DS3231 on I2C.
//
// Optional in every sense.  Compiled out unless RTC is 1, and when it is
// compiled in a missing module is not an error -- the probe at boot simply
// finds nothing, says so, and the clock free-runs exactly as it did before.
//
// What it buys: the time survives a power cut, and survives the network
// going away permanently.  Set it once and the head knows what time it is
// for the life of the coin cell, with or without WiFi ever being configured.
//
// UTC is what goes in the chip.  The timezone lives in timekeeping.h and is
// applied on the way out, so a board that is unplugged in February and
// powered up in July still shows the right hour -- which storing local time
// could not manage without a network to correct it.

#ifndef RTC_H
#define RTC_H

#include "config.h"

#if RTC

#include <Print.h>
#include <stdint.h>
#include <time.h>

// Probe the bus and, if a chip answers, read the time into the system clock.
// Call once at boot, before the network: NTP outranks the RTC, so a later
// sync overrules this without anything having to sequence them.
void rtcBegin(void);

// Whether a chip answered at RTC_ADDR when we last looked.
bool rtcPresent(void);

// Whether the chip's oscillator has run continuously since it was last set.
// False means the battery died or it has never been set, and whatever it is
// holding is not a time anyone should believe.
bool rtcValid(void);

// Read UTC out of the chip.  False if absent, invalid, or the I2C
// transaction failed.
bool rtcRead(time_t &utc);

// Write a UTC epoch into the chip, clearing the oscillator-stop flag.
bool rtcWrite(time_t utc);

// Write whatever the system clock currently holds.  Convenience for the two
// callers that want it: a completed NTP sync, and `clock set`.
bool rtcWriteNow(void);

// Chip temperature, in quarter-degree steps internally, returned in degrees
// Celsius.  The DS3231 measures it to compensate its own crystal; it comes
// out nearly free and is a good indication the bus is really working.
bool rtcTemperature(float &celsius);

// One block for the console.
void rtcReport(Print &out);

#endif // RTC
#endif // RTC_H
