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

// Named timezones, so nobody has to type a POSIX string from memory.  Named
// after cities the way the IANA database is, and grouped by region only so a
// picker can offer sixty of them without being a wall of text.
struct TzChoice {
  const char *name;
  const char *region;
  const char *posix;
};
extern const TzChoice tzChoices[];
extern const uint8_t numTzChoices;

// Returns the POSIX string for a name, or NULL if unknown.  Accepts the
// older regional names (`pacific`, `eastern`, ...) as well as the city ones.
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
void netHide(void);
bool netShowing(void);

// Set while the cards are up; the renderer skips the eyes until it passes.
extern uint32_t netShowUntil;

// RUNTIME WIFI CHANGES ------------------------------------------------------
// Each of these stores something and asks for a reboot, rather than tearing
// the link down underneath the caller.  Reconnecting in place would mean
// re-running mDNS, SNTP, the web server and OTA and getting every one of them
// idempotent; rebooting reuses setupNetwork(), which is the path that already
// works.  The board is back in a couple of seconds.
//
// The work happens in netPollPending(), called once per frame, so the HTTP
// response is on the wire before the radio goes anywhere.

// The network the radio has in NVS, which is not the same question as which
// one it is on right now.  False, and out emptied, if there is none.
bool netStoredSsid(char *out, size_t n);

// Validates and queues.  False if the SSID is empty or either is too long.
bool netRequestJoin(const char *ssid, const char *pass);

// Forget the stored network.  The build-time credentials, if any, still
// apply on the next boot -- this clears what the portal or a join stored.
void netRequestForget(void);

// Open the setup portal on the next boot, whatever is stored.
void netRequestPortal(void);

// True once one of the above has been asked for and the reboot is pending.
bool netRebootPending(void);

// Applies whatever was queued.  Called once per frame from frame().
void netPollPending(void);

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
