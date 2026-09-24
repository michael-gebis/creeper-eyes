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

void authBegin(WebServer &s) {
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

static Verdict judge(WebServer &s) {
#if AUTH_HOST_CHECK
  if (!hostIsOurs(s.hostHeader()))
    return WRONG_HOST;
#endif

#if AUTH_TOKEN
  if (s.hasHeader("Authorization") && tokenMatches(s.header("Authorization")))
    return ALLOW;
#endif

#if AUTH_HTTP
  if (!s.authenticate(credGet(CRED_USER), credGet(CRED_PASS)))
    return NEED_DIGEST;
#endif

#if AUTH_TOKEN && !AUTH_HTTP
  return NEED_TOKEN; // token is the only credential, and it did not match
#endif

  (void)s;
  return ALLOW;
}

bool authPermits(WebServer &s) { return judge(s) == ALLOW; }

bool authCheck(WebServer &s) {
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
    s.requestAuthentication(DIGEST_AUTH, WIFI_HOSTNAME,
                            "authentication required\n");
    return false;
  case NEED_TOKEN:
    s.send(401, "text/plain", "a bearer token is required\n");
    return false;
  }
  return false;
}

#endif // NETWORK
