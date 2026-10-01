// Who is allowed to drive the head.
//
// All of it is optional and all of it is off by default, because the common
// case is a prop on a home network where the only thing that can reach it is
// something you already let onto your WiFi.  Simplicity is a legitimate
// choice here, so making that choice costs nothing: with the switches off
// this module compiles to nothing at all.
//
// There is no HTTPS.  Not for want of a certificate -- you can get a real one
// for a private address through DNS-01 -- but because the Arduino core has no
// TLS server, and because a handshake is one to two seconds of ESP32 CPU on a
// web server that is polled from inside the render loop.  Every page load
// would visibly stall the eyes.  Digest authentication is the answer to the
// same question that does not cost that: the password is never sent, only a
// hash of it with a server nonce.
//
// What that does not protect against: a page you visit can make your own
// browser issue requests to this device, and a browser holding cached
// credentials will attach them.  Two ways in, two defences:
//
//  - DNS rebinding, where the page's own name is pointed at 192.168.x.x.
//    AUTH_HOST_CHECK refuses a Host header that does not name this device.
//  - A plain cross-site request to the device's real name or address, which
//    the Host check cannot tell from the page's own requests.  Whenever a
//    credential is required, a request whose browser says another site sent
//    it -- Sec-Fetch-Site, or Origin, or Referer -- is refused.  See judge().

#ifndef AUTH_H
#define AUTH_H

#include "config.h"

#if NETWORK

#include <WebServer.h>

// The web server, with digest challenges done properly.
//
// The library's requestAuthentication() makes a new nonce every time it
// challenges anybody, which silently invalidates the nonce every *other*
// browser has cached -- and it never says `stale=TRUE`, the flag that lets a
// browser retry with the new nonce instead of asking the person for the
// password again.  So one client being challenged (a script, a second tab)
// sent every open control page to a password prompt, and a test run did it
// hundreds of times.
//
// Here the nonce lives for AUTH_NONCE_S and is then retired, whoever is
// asking; and a request that carries any nonce but the current one is
// challenged with stale=TRUE, so the browser retries by itself.  Only a
// wrong password, presented with the current nonce, reaches the person.
class AuthWebServer : public WebServer {
public:
  using WebServer::WebServer;

  // A 401 with a digest challenge, stale=TRUE if the request's own nonce
  // was merely out of date.
  void digestChallenge(const char *realm, const char *message);

  // Replaces the nonce once it has lived AUTH_NONCE_S, or if there is none
  // yet.  Called before a request is judged, so an expired one fails.
  void expireNonce(void);

  // Whether a digest response was computed for this request, and not for
  // another one.  The library checks the response against the uri="..." the
  // header carries and never compares that with the request, so one header
  // seen on the network would open every route taking the same method, for
  // as long as the nonce lives.  True for anything that is not digest.
  bool digestMatchesRequest(void);

  // The library's, but a request is only taken once all of it has arrived,
  // because taking it blocks the render loop until it has been read.  See
  // auth.cpp.
  void handleClient() override;

  // For a body streamed to a handler -- the eye file -- which waits for its
  // own bytes rather than let the library wait on the render loop.
  int pendingBytes(void) { return _currentClient.available(); }
  bool clientConnected(void) { return _currentClient.connected(); }
  // How long the library's reads wait for each byte, in milliseconds.
  void setReadTimeoutMs(unsigned long ms);
  // Ends the request now: the library's next read comes back empty and the
  // body is abandoned.  This stops the server's own connection.  Stopping
  // the copy client() returns closes nothing: the copy only lets go of its
  // share of the socket.
  void abortRequest(void);

private:
  uint32_t nonceBornMs = 0;

  // handleClient()'s parts: whether the request has all arrived, and how a
  // request is turned away before the library has read any of it.
  enum Arrival { WAITING, READY, TOO_SLOW, HEAD_TOO_BIG, BODY_TOO_BIG, BAD_HEAD };
  Arrival arrival(void);
  bool routeOf(const char *buf, int len, HTTPMethod &method, bool &streamed);
  void turnAway(int code, const char *reason);
  long settledAvail = -1;  // bytes waiting when last looked, for a long head
  uint32_t settledMs = 0;  // ...and when that count last changed
};

// True if the request may proceed.  When it may not, this has already
// answered it -- 401 with a challenge, or 403 -- so the caller returns.
bool authCheck(AuthWebServer &s);

// The same judgement, without answering.  For the one place a request has
// to be judged before its handler runs: an upload body is streamed to its
// callback during parsing, and a write to flash cannot wait until afterwards
// to find out whether it was allowed.  The handler still calls authCheck()
// to deliver the refusal.
bool authPermits(AuthWebServer &s);

// Whether any credential is required at all.  Compile-time, but exposed as a
// function so /api/v1/info can report it without the caller knowing which
// switches exist.
bool authRequired(void);

// Called once at startup, to collect the headers the checks need.
void authBegin(AuthWebServer &s);

#endif // NETWORK
#endif // AUTH_H
