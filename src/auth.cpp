// See auth.h.

#include "auth.h"

#if NETWORK

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

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

// Compared in constant time.  The timing signal from an early-exit strcmp is
// not much of a lever over a network this slow, but the fix is three lines
// and the alternative is explaining why it was not worth three lines.
static bool tokenMatches(const String &header) {
  static const char *const prefix = "Bearer ";
  if (!header.startsWith(prefix))
    return false;
  const char *got = header.c_str() + strlen(prefix);
  const char *want = AUTH_TOKEN_VALUE;
  size_t n = strlen(want);
  if (strlen(got) != n)
    return false;
  uint8_t diff = 0;
  for (size_t i = 0; i < n; i++)
    diff |= (uint8_t)(got[i] ^ want[i]);
  return diff == 0;
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

bool authCheck(WebServer &s) {
#if AUTH_HOST_CHECK
  if (!hostIsOurs(s.hostHeader())) {
    s.send(403, "text/plain",
           "this request named a host that is not this device\n");
    return false;
  }
#endif

#if AUTH_TOKEN
  if (s.hasHeader("Authorization") && tokenMatches(s.header("Authorization")))
    return true;
#endif

#if AUTH_HTTP
  if (!s.authenticate(AUTH_USER, AUTH_PASS)) {
    // Digest, so the password itself never crosses the wire.  The browser
    // handles the challenge and asks the user once.
    s.requestAuthentication(DIGEST_AUTH, WIFI_HOSTNAME,
                            "authentication required\n");
    return false;
  }
#endif

#if AUTH_TOKEN && !AUTH_HTTP
  // Token is the only credential, and it did not match.
  s.send(401, "text/plain", "a bearer token is required\n");
  return false;
#endif

  (void)s;
  return true;
}

#endif // NETWORK
