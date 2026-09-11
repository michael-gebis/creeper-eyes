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
// What that does not protect against, and what AUTH_HOST_CHECK is for: DNS
// rebinding.  A page you visit can make your own browser issue requests to
// 192.168.x.x, and a browser holding cached credentials will attach them.
// Checking that the Host header names this device is the cheap defence.

#ifndef AUTH_H
#define AUTH_H

#include "config.h"

#if NETWORK

class WebServer;

// True if the request may proceed.  When it may not, this has already
// answered it -- 401 with a challenge, or 403 -- so the caller returns.
bool authCheck(WebServer &s);

// Whether any credential is required at all.  Compile-time, but exposed as a
// function so /api/v1/info can report it without the caller knowing which
// switches exist.
bool authRequired(void);

// Called once at startup, to collect the headers the checks need.
void authBegin(WebServer &s);

#endif // NETWORK
#endif // AUTH_H
