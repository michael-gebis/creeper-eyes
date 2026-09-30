// See auth.h.

#include "auth.h"

#if NETWORK

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

#include "credentials.h"

bool authRequired(void) { return AUTH_HTTP || AUTH_TOKEN; }

#if AUTH_HOST_CHECK || AUTH_HTTP || AUTH_TOKEN

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

#endif // AUTH_HOST_CHECK || AUTH_HTTP || AUTH_TOKEN

#if AUTH_HTTP || AUTH_TOKEN

// Whether an Origin, or the start of a Referer, is this device: http:// and
// one of its own names.  "null" (a sandboxed frame, a file) is not.
static bool originIsOurs(const String &url) {
  static const char scheme[] = "http://";
  if (!url.startsWith(scheme))
    return false;
  const int start = sizeof(scheme) - 1;
  const int slash = url.indexOf('/', start);
  return hostIsOurs(slash < 0 ? url.substring(start) : url.substring(start, slash));
}

// Whether the browser says another site made it send this request -- the
// shape of cross-site request forgery, where a page somebody visits uses the
// credentials their browser holds for this device.  The Host check cannot see
// it: such a request names this device, because it is addressed to it.
//
// Current browsers send Sec-Fetch-Site on every request, top-level
// navigations included.  Older ones send Origin on anything but a GET, and
// usually a Referer.  A request with none of them is not from a browser, or
// is from one that says nothing, and its credentials decide.
static bool fromAnotherSite(AuthWebServer &s) {
  const String site = s.header("Sec-Fetch-Site");
  if (site.length()) {
    if (site == "same-origin" || site == "none") // "none": typed, or a bookmark
      return false;
    // Following a link to the control page from somewhere else is still a way
    // to open it.  It changes nothing, and everything the page then asks for
    // is same-origin.
    return !(s.method() == HTTP_GET && s.uri() == "/" &&
             s.header("Sec-Fetch-Dest") == "document");
  }
  const String origin = s.header("Origin");
  if (origin.length())
    return !originIsOurs(origin);
  const String referer = s.header("Referer");
  if (referer.length())
    return !originIsOurs(referer);
  return false;
}

#endif // AUTH_HTTP || AUTH_TOKEN

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

#if AUTH_HTTP

bool AuthWebServer::digestMatchesRequest(void) {
  String h = header("Authorization");
  if (!h.startsWith("Digest "))
    return true; // a token, or nothing: authenticate() decides
  const String uri = _extractParam(h, F("uri=\""), '"');
  const int q = uri.indexOf('?');
  if ((q < 0 ? uri : uri.substring(0, q)) != _currentUri)
    return false;
  const String query = q < 0 ? String() : uri.substring(q + 1);

  // A raw upload body (the eye file) is read without the library parsing the
  // query at all, so there is nothing to compare one with: such a request's
  // digest must name no query.
  if (_currentHandler && _currentHandler->canRaw(_currentUri))
    return !query.length();

  // Otherwise the query must decode to exactly the arguments the request
  // carried, split the way the library splits them -- a pair with no '=' is
  // dropped.  "plain" is the library's name for a non-form body, so it is
  // left out on both sides.  A form-encoded body adds arguments the digest
  // does not name, so it fails here, which nothing legitimate sends.
  int next = 0;
  auto skipPlain = [&]() {
    while (next < args() && argName(next) == "plain")
      next++;
  };
  for (int pos = 0; query.length();) {
    const int eq = query.indexOf('=', pos);
    const int amp = query.indexOf('&', pos);
    if (eq >= 0 && (amp < 0 || eq < amp)) {
      const String key = urlDecode(query.substring(pos, eq));
      if (key != "plain") {
        skipPlain();
        // amp of -1 becomes "to the end", exactly as in the library.
        if (next >= args() || argName(next) != key ||
            arg(next) != urlDecode(query.substring(eq + 1, amp)))
          return false;
        next++;
      }
    }
    if (amp < 0)
      break;
    pos = amp + 1;
  }
  skipPlain();
  return next == args();
}

#endif // AUTH_HTTP

void authBegin(AuthWebServer &s) {
#if AUTH_HTTP || AUTH_TOKEN
  // Authorization is collected by the server regardless -- it reserves the
  // first slot for it -- but asking explicitly documents the dependency and
  // survives somebody else calling collectHeaders later.  The rest are what
  // fromAnotherSite() reads.
  // Not const-qualified: collectHeaders takes a non-const array.
  static const char *wanted[] = {"Authorization", "Origin", "Referer",
                                 "Sec-Fetch-Site", "Sec-Fetch-Dest"};
  s.collectHeaders(wanted, sizeof(wanted) / sizeof(wanted[0]));
#else
  (void)s;
#endif
}

// What the checks make of a request, before anything is said about it.
enum Verdict { ALLOW, WRONG_HOST, CROSS_SITE, NEED_DIGEST, NEED_TOKEN };

static Verdict judge(AuthWebServer &s) {
#if AUTH_HOST_CHECK
  if (!hostIsOurs(s.hostHeader()))
    return WRONG_HOST;
#endif

#if AUTH_HTTP || AUTH_TOKEN
  // Before any credential, so a forged request is refused however good the
  // credential it carries.
  if (fromAnotherSite(s))
    return CROSS_SITE;
#endif

#if AUTH_TOKEN
  if (s.hasHeader("Authorization") && tokenMatches(s.header("Authorization")))
    return ALLOW;
#endif

#if AUTH_HTTP
  s.expireNonce(); // an expired nonce must fail here, not be accepted
  if (!s.authenticate(credGet(CRED_USER), credGet(CRED_PASS)) ||
      !s.digestMatchesRequest())
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
  case CROSS_SITE:
    s.send(403, "text/plain", "this request was sent by another site\n");
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
