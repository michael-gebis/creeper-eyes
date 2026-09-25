// See auth.h.

#include "auth.h"

#if NETWORK

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

#include "credentials.h"

bool authRequired(void) { return AUTH_HTTP || AUTH_TOKEN; }

#if AUTH_HOST_CHECK

// The names this board answers to.  A request arriving under any other name
// reached us through somebody else's DNS, which is the shape of a rebinding
// attack -- the browser is ours, the intent is not.
static bool hostIsOurs(const String &host) {
  if (!host.length())
    return false; // HTTP/1.1 requires it; anything omitting it is not a browser

  // Strip the port: "frank.local:80" is the same host as "frank.local".
  int colon = host.indexOf(':');
  String h = colon < 0 ? host : host.substring(0, colon);

  if (h.equalsIgnoreCase(WIFI_HOSTNAME ".local") ||
      h.equalsIgnoreCase(WIFI_HOSTNAME))
    return true;
  if (h == WiFi.localIP().toString())
    return true;
  if (h == WiFi.softAPIP().toString())
    return true; // the setup portal, before there is a LAN address
  return false;
}

#endif // AUTH_HOST_CHECK

#if AUTH_TOKEN

// "Bearer <token>", the token compared in constant time by credMatches().
static bool tokenMatches(const String &header) {
  static const char *const prefix = "Bearer ";
  if (!header.startsWith(prefix))
    return false;
  return credMatches(CRED_TOKEN, header.c_str() + strlen(prefix));
}

#endif // AUTH_TOKEN

// 128 random bits as hex, from the hardware RNG -- which is truly random
// once the radio is up, and this is only ever called after it is.
static String randomHex(void) {
  char buf[33];
  for (uint8_t i = 0; i < 4; i++)
    snprintf(buf + i * 8, 9, "%08lx", (unsigned long)esp_random());
  return String(buf);
}

void AuthWebServer::expireNonce(void) {
  if (_snonce.length() &&
      millis() - nonceBornMs < (uint32_t)AUTH_NONCE_S * 1000UL)
    return;
  _snonce = randomHex();
  _sopaque = randomHex();
  nonceBornMs = millis();
}

// The nonce="..." a request's Authorization header carries, or "".
static String requestNonce(WebServer &s) {
  if (!s.hasHeader("Authorization"))
    return "";
  const String h = s.header("Authorization");
  if (!h.startsWith("Digest "))
    return "";
  int at = h.indexOf("nonce=\"");
  if (at < 0)
    return "";
  at += 7;
  int end = h.indexOf('"', at);
  return end < 0 ? String() : h.substring(at, end);
}

// requestAuthentication(), less the fresh nonce on every call and plus the
// stale flag -- see auth.h.
void AuthWebServer::digestChallenge(const char *realm, const char *message) {
  expireNonce();
  _srealm = realm;
  const String theirs = requestNonce(*this);
  const bool stale = theirs.length() && theirs != _snonce;
  sendHeader("WWW-Authenticate",
             String("Digest realm=\"") + _srealm + "\", qop=\"auth\", nonce=\"" +
                 _snonce + "\", opaque=\"" + _sopaque + "\"" +
                 (stale ? ", stale=TRUE" : ""));
  send(401, "text/plain", message);
}

void authBegin(AuthWebServer &s) {
#if AUTH_TOKEN
  // Authorization is collected by the server regardless -- it reserves the
  // first slot for it -- but asking explicitly documents the dependency and
  // survives somebody else calling collectHeaders later.
  // Not const-qualified: collectHeaders takes a non-const array.
  static const char *wanted[] = {"Authorization"};
  s.collectHeaders(wanted, 1);
#else
  (void)s;
#endif
}

// What the checks make of a request, before anything is said about it.
enum Verdict { ALLOW, WRONG_HOST, NEED_DIGEST, NEED_TOKEN };

static Verdict judge(AuthWebServer &s) {
#if AUTH_HOST_CHECK
  if (!hostIsOurs(s.hostHeader()))
    return WRONG_HOST;
#endif

#if AUTH_TOKEN
  if (s.hasHeader("Authorization") && tokenMatches(s.header("Authorization")))
    return ALLOW;
#endif

#if AUTH_HTTP
  s.expireNonce(); // an expired nonce must fail here, not be accepted
  if (!s.authenticate(credGet(CRED_USER), credGet(CRED_PASS)))
    return NEED_DIGEST;
#endif

#if AUTH_TOKEN && !AUTH_HTTP
  return NEED_TOKEN; // token is the only credential, and it did not match
#endif

  (void)s;
  return ALLOW;
}

bool authPermits(AuthWebServer &s) { return judge(s) == ALLOW; }

bool authCheck(AuthWebServer &s) {
  switch (judge(s)) {
  case ALLOW:
    return true;
  case WRONG_HOST:
    s.send(403, "text/plain",
           "this request named a host that is not this device\n");
    return false;
  case NEED_DIGEST:
    // Digest, so the password itself never crosses the wire.  The browser
    // handles the challenge and asks the user once.
    s.digestChallenge(WIFI_HOSTNAME, "authentication required\n");
    return false;
  case NEED_TOKEN:
    s.send(401, "text/plain", "a bearer token is required\n");
    return false;
  }
  return false;
}

#endif // NETWORK
