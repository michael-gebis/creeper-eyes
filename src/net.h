// WiFi, the setup portal, mDNS and network time.
//
// Everything here compiles to nothing when NETWORK is 0, which is what the
// *_local environments in platformio.ini build.  Nothing outside this module
// and web.cpp knows the network exists.

#ifndef NET_H
#define NET_H

#include "config.h"

#if NETWORK

#include <Print.h>
#include <stdint.h>

// Tri-state so callers can tell "never connected" from "gave up".
enum { NET_DOWN, NET_UP, NET_PORTAL };
extern uint8_t netState;

// Whether NTP has ever answered.  The clock free-runs until it has.
extern bool timeSynced;
extern char tzString[TZ_MAX];

// Named timezones, so nobody has to type a POSIX string from memory.
struct TzChoice {
  const char *name;
  const char *posix;
};
extern const TzChoice tzChoices[];
extern const uint8_t numTzChoices;

// Returns the POSIX string for a shortcut name, or NULL if unknown.
const char *tzLookup(const char *name);

// Connect, or open the portal.  Blocks; may take the portal timeout.
void setupNetwork(void);

// Bring up mDNS, IPv6 and SNTP.  Call once a link exists.
void netOnConnected(void);

// Apply tzString and (re)start SNTP.  Safe to call again after a change.
void netStartTime(void);

// Poll for the first successful sync.  Cheap no-op afterwards.
void netPollTime(void);

// Human-readable addresses and time.
void netReport(Print &out);

// Paint the address cards on the panels for a while.  Non-blocking.
void netShow(void);

// Set while the cards are up; the renderer skips the eyes until it passes.
extern uint32_t netShowUntil;

// The web interface and OTA, in web.cpp.
void webBegin(void);
void otaBegin(void);
void webPoll(void);

// The REST API, in api.cpp.  Hangs its routes off the server the web
// module owns, rather than owning one of its own.
class WebServer;
void apiRegister(WebServer &s);

#endif // NETWORK
#endif // NET_H
