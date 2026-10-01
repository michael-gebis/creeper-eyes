// See auth.h.

#include "auth.h"

#if NETWORK

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <lwip/sockets.h>

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

// TAKING A REQUEST ----------------------------------------------------------
//
// The library takes a request the moment its first byte arrives, then reads
// the rest -- the request line, every header, the body -- waiting for each
// byte as it comes.  This server runs on the render loop (web.cpp), so for as
// long as a request takes to arrive the eyes are frozen and nobody else is
// served, and nothing bounds that: a client sending a byte every few seconds
// holds the loop for as long as it likes, before any credential has been
// looked at.  What it reads it keeps, too -- a header line or a body of any
// length -- on the heap the rest of the firmware shares.
//
// So a new client waits, at the cost of a peek each frame, until its whole
// request is already in the socket: the head up to its blank line, and the
// body by its Content-Length.  Then every read the library makes finds its
// byte waiting, and the request is served at once.  A request that would not
// fit is refused before any of it is read -- a head past REQUEST_HEAD_MAX, a
// body past REQUEST_BODY_MAX -- and one that has not all arrived after
// REQUEST_ARRIVE_MS is dropped.  The one body exempt is a route that streams
// its body to a handler, the eye file, which bounds its own time instead
// (api.cpp, eyeSlotBody).

static char peekBuf[REQUEST_HEAD_MAX];

// The library's method names, for finding the route a request will take.
static HTTPMethod methodNamed(const char *s, int len) {
  static const struct {
    const char *name;
    HTTPMethod method;
  } known[] = {{"GET", HTTP_GET},         {"POST", HTTP_POST},
               {"PUT", HTTP_PUT},         {"DELETE", HTTP_DELETE},
               {"PATCH", HTTP_PATCH},     {"OPTIONS", HTTP_OPTIONS},
               {"HEAD", HTTP_HEAD}};
  for (const auto &k : known)
    if ((int)strlen(k.name) == len && !strncmp(s, k.name, len))
      return k.method;
  return HTTP_ANY;
}

// Where a header's value starts in the head, or -1.  Case-insensitive, and
// only at the start of a line.
static int headerValueAt(const char *head, int len, const char *name) {
  const int n = strlen(name);
  for (int i = 0; i + 2 + n < len; i++) {
    if (head[i] != '\r' || head[i + 1] != '\n' ||
        strncasecmp(head + i + 2, name, n) || head[i + 2 + n] != ':')
      continue;
    int v = i + 3 + n;
    while (v < len && head[v] == ' ')
      v++;
    return v;
  }
  return -1;
}

// How long a client must have stopped sending before a head too long to see
// whole is taken -- see arrival().  Its segments come back to back.
#ifndef REQUEST_SETTLE_MS
#define REQUEST_SETTLE_MS 50
#endif

// From a request line at the start of buf: its method, and whether the route
// the library will give it streams its body to a handler.  The first handler
// that takes it, as the library chooses, on the path without its query.
// False if there is no whole request line yet.
bool AuthWebServer::routeOf(const char *buf, int len, HTTPMethod &method,
                            bool &streamed) {
  const char *eol = (const char *)memchr(buf, '\r', len);
  const char *sp1 = (const char *)memchr(buf, ' ', eol ? eol - buf : len);
  const char *sp2 = sp1 ? (const char *)memchr(sp1 + 1, ' ', (eol ? eol : buf + len) - sp1 - 1)
                        : nullptr;
  if (!sp2)
    return false;
  method = methodNamed(buf, sp1 - buf);
  String path(sp1 + 1, sp2 - sp1 - 1); // String(const char *, unsigned int)
  const int q = path.indexOf('?');
  if (q >= 0)
    path.remove(q);
  streamed = false;
  for (RequestHandler *h = _firstHandler; h; h = h->next())
    if (h->canHandle(method, path)) {
      streamed = h->canRaw(path);
      break;
    }
  return true;
}

AuthWebServer::Arrival AuthWebServer::arrival(void) {
  const int fd = _currentClient.fd();
  if (fd < 0)
    return TOO_SLOW;
  // Nothing has been read from a new client, so its whole request so far is
  // in the socket, and a peek sees it without taking any of it -- as far as
  // the first segment that arrived, which is all lwIP will show a peek.
  int n = recv(fd, peekBuf, sizeof(peekBuf), MSG_PEEK | MSG_DONTWAIT);
  if (n < 0)
    n = 0;
  const unsigned long avail = _currentClient.available();
  const bool late = millis() - _statusChange > REQUEST_ARRIVE_MS;

  int headLen = -1;
  for (int i = 0; i + 3 < n; i++)
    if (!memcmp(peekBuf + i, "\r\n\r\n", 4)) {
      headLen = i + 4;
      break;
    }
  HTTPMethod method = HTTP_ANY;
  bool streamed = false;

  if (headLen < 0) {
    // All there is, and not a whole head yet.  Or nothing seen at all: lwIP
    // can count a segment's bytes before a peek can see them, so for a
    // moment the peek fails (EAGAIN) while available() says hundreds of
    // bytes are waiting.  Measured: five milliseconds later it saw them all.
    // Either way, look again next frame.
    if (n == 0 || avail <= (unsigned long)n)
      return n >= (int)sizeof(peekBuf) ? HEAD_TOO_BIG : late ? TOO_SLOW : WAITING;

    // More has arrived than the peek can see: a head too long for one
    // segment.  Its size is known only as what has arrived, so the limits
    // apply to that.  Once the client stops sending, the library reads it
    // without waiting for anything (see handleClient()), so a head that was
    // not in fact whole fails at once instead of waiting on the render loop.
    if (!routeOf(peekBuf, n, method, streamed))
      return BAD_HEAD;
    const bool bodiless = method == HTTP_GET || method == HTTP_HEAD ||
                          method == HTTP_OPTIONS;
    if (!streamed && avail > (unsigned long)REQUEST_HEAD_MAX +
                                 (bodiless ? 0 : REQUEST_BODY_MAX))
      return bodiless ? HEAD_TOO_BIG : BODY_TOO_BIG;
    if ((long)avail != settledAvail) {
      settledAvail = avail;
      settledMs = millis();
    } else if (millis() - settledMs >= REQUEST_SETTLE_MS) {
      return READY;
    }
    return late ? TOO_SLOW : WAITING;
  }

  if (!routeOf(peekBuf, headLen, method, streamed))
    return BAD_HEAD;
  // A multipart body is never streamed: the library parses those itself.
  const int ct = headerValueAt(peekBuf, headLen, "Content-Type");
  if (ct >= 0 && !strncasecmp(peekBuf + ct, "multipart/", 10))
    streamed = false;

  unsigned long body = 0;
  const int cl = headerValueAt(peekBuf, headLen, "Content-Length");
  if (cl >= 0) {
    int digits = 0;
    for (int i = cl; i < headLen && isdigit((unsigned char)peekBuf[i]); i++, digits++)
      body = body * 10 + (peekBuf[i] - '0');
    if (!digits || digits > 9)
      return BAD_HEAD;
  }
  if (streamed)
    return READY; // its handler waits for the body itself
  if (body > REQUEST_BODY_MAX)
    return BODY_TOO_BIG;
  if (avail >= headLen + body)
    return READY;
  return late ? TOO_SLOW : WAITING;
}

void AuthWebServer::turnAway(int code, const char *reason) {
  _currentClient.printf("HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\n"
                        "Content-Length: %u\r\nConnection: close\r\n\r\n%s\n",
                        code, reason, (unsigned)strlen(reason) + 1, reason);
  // Take what has already arrived, so closing sends the client an end of
  // stream after the reply rather than a reset, which some clients answer by
  // throwing the reply away unread.
  for (int n = _currentClient.available(); n > 0; n = _currentClient.available())
    if (_currentClient.read((uint8_t *)peekBuf, n < (int)sizeof(peekBuf) ? n : sizeof(peekBuf)) <= 0)
      break;
  _currentClient = WiFiClient(); // lets go of the socket, which closes it
  _currentStatus = HC_NONE;
}

void AuthWebServer::handleClient() {
  if (_currentStatus == HC_NONE) {
    _currentClient = _server.available();
    if (!_currentClient) {
      if (_nullDelay)
        delay(1);
      return;
    }
    _currentStatus = HC_WAIT_READ;
    _statusChange = millis();
    settledAvail = -1;
  }
  if (_currentStatus == HC_WAIT_READ && _currentClient.connected()) {
    switch (arrival()) {
    case WAITING:
      return; // look again next frame; the eyes carry on meanwhile
    case READY:
      // Everything the library will read is already here, so its reads need
      // not wait for anything.  They waited five seconds a byte before: the
      // library sets that for sending each response, and WiFiClient's
      // assignment does not copy the timeout, so every client after the first
      // was read with it.  (A streamed body waits for its own bytes.)
      setReadTimeoutMs(1);
      break;
    case TOO_SLOW:
      _currentClient = WiFiClient();
      _currentStatus = HC_NONE;
      return;
    case HEAD_TOO_BIG:
      turnAway(431, "Request Header Fields Too Large");
      return;
    case BODY_TOO_BIG:
      turnAway(413, "Payload Too Large");
      return;
    case BAD_HEAD:
      turnAway(400, "Bad Request");
      return;
    }
  }
  WebServer::handleClient();
}

void AuthWebServer::setReadTimeoutMs(unsigned long ms) {
  // Stream's, which is what the library's reads wait on.  WiFiClient's own
  // setTimeout takes seconds and also sets the socket's send timeout.
  static_cast<Stream &>(_currentClient).setTimeout(ms);
}

void AuthWebServer::abortRequest(void) {
  setReadTimeoutMs(0);   // the library's next read gives up at once...
  _currentClient.stop(); // ...and finds nothing there
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
