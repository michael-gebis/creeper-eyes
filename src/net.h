// WiFi, the setup portal, mDNS and NTP.
//
// Everything here compiles to nothing when NETWORK is 0, which is what the
// *_local environments in platformio.ini build.  Nothing outside this module
// and web.cpp knows the network exists.
//
// The timezone and the system clock are not here: they belong to
// timekeeping.h, which is compiled either way, because an RTC needs both and
// does not need a network.  This module is one source feeding that one.

#ifndef NET_H
#define NET_H

#include "config.h"

#if NETWORK

#include "timekeeping.h"

#include <Print.h>
#include <stdint.h>

// Tri-state so callers can tell "never connected" from "gave up".
enum { NET_DOWN, NET_UP, NET_PORTAL };
extern uint8_t netState;

// Connect, or open the portal.  Blocks; may take the portal timeout.
void setupNetwork(void);

// Bring up mDNS and SNTP, and IPv6 if it is compiled in.  Call once a link
// exists.
void netOnConnected(void);

// (Re)start SNTP with the current timezone.  Safe to call again after a
// change; the servers are re-resolved and the next reply is accepted.
void netStartTime(void);

// Hand a completed sync to timekeeping, and write it through to the RTC.
// Called once per frame; a flag check until a reply actually lands.
void netPollTime(void);

// What the SNTP client is doing, for anything that wants to report it rather
// than just consume the time.
struct NtpStatus {
  bool enabled;         // the saved setting: should we ask at all?
  bool running;         // the client is started
  bool linkUp;          // ...and there is a network for it to ask over
  bool synced;          // a server has answered at least once
  uint32_t lastSyncSec; // seconds since that answer; NTP_NEVER if none
  uint32_t intervalSec; // how often it asks again
  const char *server;
};
#define NTP_NEVER 0xFFFFFFFFUL

void netNtpStatus(NtpStatus &out);

// Ask again now rather than waiting out the interval.  False if the client
// is not running, which means there is nothing to restart.
bool netNtpSyncNow(void);

// Whether to use a time server at all.  Saved, and on by default.  Turning it
// off stops the client but leaves the time it already supplied alone: the
// clock does not become wrong just because we stopped asking.  For a head
// with an RTC, or one whose time is set by hand, this is how you stop a
// server three hours from now quietly overruling you.
bool netNtpEnabled(void);
void netNtpSetEnabled(bool on);

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
