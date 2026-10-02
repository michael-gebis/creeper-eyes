// The REST API.  JSON in, JSON out, over /api/v1.
//
// Every handler is a thin translation: parse a body, call one operation --
// from state.h for anything about the eyes, from net.h for anything about the
// network -- and report what happened.  No device logic lives here, which is
// what keeps it honestly in step with the serial console: both front ends
// drive the same operations, so neither can grow behaviour the other lacks.
//
// Versioned from the start because this interface has external clients by
// design.  When something has to change incompatibly, /api/v2 can appear
// beside v1 rather than breaking whatever is already talking to it.

#include "config.h"

#if NETWORK

#include "auth.h"
#include "credentials.h"
#include "sleepmode.h"
#include "display.h"
#include "net.h"
#include "rtc.h"
#include "timekeeping.h"
#include "state.h"
#include "health.h"
#include "parse.h"
#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>

// The server is owned by web.cpp; this module only hangs routes off it.
// The prefix every route in this file hangs off.  Defined here rather
// than beside the route table because guarded() below compares against
// it.
#define API "/api/v1"

static AuthWebServer *srv = NULL;

// --------------------------------------------------------------- plumbing --

// Wraps a handler in the credential check.  Registering guarded<getState>
// instead of getState means a route added later cannot quietly miss the
// guard: there is one place a route is named, and the guard is part of
// naming it.  Compiles away to nothing when no credential is required.
template <void (*H)()> static void guarded(void) {
  if (!authCheck(*srv))
    return;
#if SLEEP
  // Anything that changes state counts as someone being there, and wakes the
  // eyes for a while.  Two exceptions.
  //
  // Reads do not count: the control page polls once a second while it is
  // open, so counting GET would mean a forgotten browser tab kept the head
  // awake all night.
  //
  // Nor do the sleep settings themselves.  Configuring the schedule is
  // administration rather than presence, and a request that says "sleep from
  // now" must not also say "and someone is here, so stay up" -- which would
  // leave the board awake for a minute and look like the setting did nothing.
  if (srv->method() != HTTP_GET && srv->method() != HTTP_OPTIONS &&
      srv->uri() != API "/sleep")
    sleepNudge();
#endif
  H();
}

// Browsers refuse cross-origin requests without these, which would stop a
// page served from anywhere else driving the device.
//
// Wide open while nothing is required, which is the point -- anything on the
// network is welcome to drive an unauthenticated prop.  Once a credential is
// needed the wildcard is wrong twice over: browsers reject it alongside
// credentials, and inviting arbitrary origins to send them is the opposite of
// what turning authentication on asked for.
static void corsHeaders(void) {
  if (!authRequired())
    srv->sendHeader("Access-Control-Allow-Origin", "*");
  srv->sendHeader("Access-Control-Allow-Methods",
                "GET, PUT, POST, DELETE, OPTIONS");
  srv->sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

static void sendJson(int code, JsonDocument &doc) {
  String body;
  serializeJson(doc, body);
  corsHeaders();
  srv->send(code, "application/json", body);
}

static void sendError(int code, const char *message) {
  JsonDocument d;
  d["error"] = message;
  sendJson(code, d);
}

static void sendOk(void) {
  JsonDocument d;
  d["ok"] = true;
  sendJson(200, d);
}

// A PUT or POST body, parsed.  Returns false and answers with 400 if it is
// missing or malformed, so callers can simply return.
static bool readBody(JsonDocument &doc) {
  // WebServer only keeps the raw body under "plain" when it did not recognise
  // the content type; a form-encoded body has already been split into args by
  // the time a handler runs, which is the usual reason for landing here.
  if (!srv->hasArg("plain")) {
    sendError(400, "expected a JSON body with Content-Type: application/json");
    return false;
  }
  DeserializationError e = deserializeJson(doc, srv->arg("plain"));
  if (e) {
    sendError(400, "malformed JSON");
    return false;
  }
  // Every body this API takes is an object.  A bare array or number parses,
  // and every field lookup on it then quietly finds nothing.
  if (!doc.is<JsonObject>()) {
    sendError(400, "expected a JSON object");
    return false;
  }
  return true;
}

// Whether `key` is in the body with a type other than T.  That is an error,
// not something to skip: {"rate":"5"} was sent to change the rate, and
// ignoring it would report success for a change that never happened.  Absent
// is fine -- every PUT here changes only what it is sent.
//
// Answers 400, naming the field, and returns true, so a handler checks every
// field it knows before it changes anything:
//
//   if (wrongType<bool>(b, "on", "true or false")) return;
template <typename T>
static bool wrongType(JsonDocument &b, const char *key, const char *expected) {
  JsonVariantConst v = b[key];
  if (v.isNull() || v.is<T>())
    return false;
  String msg = String(key) + " must be " + expected;
  sendError(400, msg.c_str());
  return true;
}

// A known resource reached with the wrong verb is a 405, not a 404.
// Registered last for each path: WebServer matches in registration
// order, so the specific methods above win and this catches the rest.
static void notAllowed(void) {
  sendError(405, "method not allowed on this resource");
}

// Preflight.  Answered for every mutable route.
static void handleOptions(void) {
  corsHeaders();
  srv->send(204);
}

// ------------------------------------------------------------ serialising --

static void fillEye(JsonObject o, const DeviceState &s) {
  o["index"] = s.eyeIndex;
  o["name"] = s.eyeName;
  o["count"] = s.eyeCount;
}

static void fillGaze(JsonObject o, const DeviceState &s) {
  o["mode"] = s.gazeManual ? "manual" : "auto";
  o["x"] = s.gazeX;
  o["y"] = s.gazeY;
}

static void fillDilate(JsonObject o, const DeviceState &s) {
  o["mode"] = s.dilateManual ? "manual" : "auto";
  o["percent"] = s.dilatePercent;
}

static void fillClock(JsonObject o, const DeviceState &s) {
  o["on"] = s.clockOn;
  // Switched on, but with nothing to draw: no server, no RTC, and nobody has
  // typed the time in, so a clock face would be a guess.
  o["suppressed"] = s.clockSuppressed;
  o["seconds"] = s.clockSeconds;
  o["rate"] = s.clockRate;
  o["secondOfDay"] = s.clockSecOfDay;
  char buf[9];
  snprintf(buf, sizeof(buf), "%02u:%02u:%02u",
           (unsigned)(s.clockSecOfDay / 3600), (unsigned)((s.clockSecOfDay / 60) % 60),
           (unsigned)(s.clockSecOfDay % 60));
  o["time"] = buf;
  JsonObject c = o["colors"].to<JsonObject>();
  static const char *const names[3] = {"hour", "minute", "second"};
  for (uint8_t i = 0; i < 3; i++) {
    snprintf(buf, sizeof(buf), "%06lX", (unsigned long)s.clockColor[i]);
    c[names[i]] = buf;
  }
}

static void fillNet(JsonObject o) {
  o["hostname"] = WIFI_HOSTNAME;
  o["mdns"] = WIFI_HOSTNAME ".local";
  o["mac"] = WiFi.macAddress();
  bool up = WiFi.status() == WL_CONNECTED;
  o["state"] = up ? "up" : (netState == NET_PORTAL ? "portal" : "down");
  if (up) {
    o["ssid"] = WiFi.SSID();
    o["rssi"] = WiFi.RSSI();
    o["ipv4"] = WiFi.localIP().toString();
#if IPV6
    o["ipv6"] = WiFi.localIPv6().toString();
#endif
    o["gateway"] = WiFi.gatewayIP().toString();
  }
  // Reported unconditionally, and false while IPV6 is 0 -- see config.h.
  // A client that gets no ipv6 field should be able to find out why without
  // guessing.
  o["ipv6Served"] = (bool)IPV6;
  o["timeSynced"] = timeSynced;
  o["tz"] = tzString;
  o["showingInfo"] = netShowing();
}

// ---------------------------------------------------------------- handlers --

// Everything about where the time comes from, in one place, so the control
// page can show it without a second request against a server that handles one
// client at a time.
static void fillTime(JsonObject o) {
  o["source"] = timeSourceName(); // ntp | rtc | manual | free

  NtpStatus n;
  netNtpStatus(n);
  JsonObject jn = o["ntp"].to<JsonObject>();
  jn["available"] = true; // this code only exists in a NETWORK build
  jn["enabled"] = n.enabled;
  jn["running"] = n.running;
  jn["linkUp"] = n.linkUp;
  jn["synced"] = n.synced;
  if (n.lastSyncSec != NTP_NEVER)
    jn["lastSyncSeconds"] = n.lastSyncSec;
  jn["intervalSeconds"] = n.intervalSec;
  jn["server"] = n.server;

  JsonObject jr = o["rtc"].to<JsonObject>();
  jr["enabled"] = (bool)RTC;
#if RTC
  jr["present"] = rtcPresent();
  jr["valid"] = rtcValid();
  // Seven registers over I2C, which is nothing beside the cost of answering
  // the request itself -- and a chip time that disagrees with the system
  // clock is exactly what someone reading this page wants to find out.
  time_t utc;
  if (rtcRead(utc)) {
    struct tm g;
    gmtime_r(&utc, &g);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &g);
    jr["utc"] = buf;
  }
  float c;
  if (rtcTemperature(c))
    jr["temperatureC"] = c;
#else
  jr["present"] = false;
  jr["valid"] = false;
#endif
}

static void fillDim(JsonObject o, const DeviceState &s) {
  o["percent"] = s.dimPercent;
  o["shown"] = s.dimShown; // differs during a fade, a sweep, or sleep
  o["gamma"] = s.dimGammaX10 / 10.0;
  JsonObject t = o["trim"].to<JsonObject>();
  t["left"] = s.dimTrim[0];
  t["right"] = s.dimTrim[1];
  o["sweeping"] = s.dimSweeping;
}

static void fillFlip(JsonObject o, const DeviceState &s) {
  o["left"] = s.flipped[0];
  o["right"] = s.flipped[1];
}

static void getState(void) {
  DeviceState s;
  stateGet(s);
  JsonDocument d;
  fillEye(d["eye"].to<JsonObject>(), s);
  fillGaze(d["gaze"].to<JsonObject>(), s);
  fillDilate(d["dilate"].to<JsonObject>(), s);
  d["pupil"]["on"] = s.pupilOn;
  d["swap"]["on"] = s.swapped;
  fillFlip(d["flip"].to<JsonObject>(), s);
  fillDim(d["dim"].to<JsonObject>(), s);
  d["startle"]["active"] = s.startleActive;
  fillClock(d["clock"].to<JsonObject>(), s);
  fillNet(d["net"].to<JsonObject>());
  fillTime(d["time"].to<JsonObject>());
  JsonObject sys = d["system"].to<JsonObject>();
  // The panel type matters to a client: on a greyscale panel every colour
  // it sets will come back as a brightness.
#if USE_SSD1327
  sys["panel"] = "ssd1327";
#else
  sys["panel"] = "ssd1351";
#endif
  sys["panels"] = displayCount();
#if SLEEP
  // Compact, because /state is polled once a second: whether it is dark, why,
  // and how long until that changes, with the settings left to GET /sleep.
  //
  // The countdown lives here and not only in /sleep because the page renders
  // the card from this once a second.  It used to merge a live `reason` from
  // here with a countdown fetched from /sleep at page load, and a tab left
  // open across 22:00 read "asleep -- sleeps in 2h 4m".  One source, one
  // moment: the two halves cannot disagree if they came from the same call.
  JsonObject slp = d["sleep"].to<JsonObject>();
  slp["enabled"] = sleepEnabled();
  slp["asleep"] = sleepIsAsleep();
  slp["reason"] = sleepReason();
  uint16_t mins;
  bool toAsleep;
  if (sleepNextChange(mins, toAsleep)) {
    slp["changesInMinutes"] = mins;
    slp["changesToAsleep"] = toAsleep;
  }
#endif
  sys["fps"] = s.fps;
  sys["cpuMhz"] = s.cpuMhz;
  sys["cpuSetting"] = s.cpuSetting;
  sys["freeHeap"] = s.freeHeap;
  sys["uptimeSeconds"] = s.uptimeSec;
  sys["settingsDirty"] = s.settingsDirty;
  // What the board found wrong with its stored data at boot -- see
  // health.h.  Usually empty, so it costs a few bytes on a reply that is
  // polled once a second, and it saves the page a second request.
  JsonArray warn = sys["warnings"].to<JsonArray>();
  for (uint8_t i = 0; i < healthCount(); i++)
    warn.add(healthLine(i));
  if (healthDropped()) {
    char more[32];
    snprintf(more, sizeof(more), "...and %u more", (unsigned)healthDropped());
    warn.add(more);
  }
  sendJson(200, d);
}

static void fillEyeSlot(JsonObject o, const EyeSlotState &e) {
  o["available"] = e.available;
  o["loaded"] = e.loaded;
  if (e.loaded) {
    o["name"] = e.name;
    o["index"] = e.index;
  }
  o["capacity"] = e.capacity;
}

static void getEyes(void) {
  DeviceState s;
  stateGet(s);
  EyeSlotState slot;
  stateEyeSlot(slot);
  JsonDocument d;
  JsonArray a = d["designs"].to<JsonArray>();
  for (uint8_t i = 0; i < stateEyeCount(); i++) {
    JsonObject o = a.add<JsonObject>();
    o["index"] = i;
    o["name"] = stateEyeName(i);
    o["current"] = (i == s.eyeIndex);
    o["loaded"] = (slot.loaded && i == slot.index);
  }
  fillEyeSlot(d["slot"].to<JsonObject>(), slot);
  sendJson(200, d);
}

// ------------------------------------------------------------- eye slot --
// One design loaded from a file; see docs/EYE_FILES.md.
//
// The upload is the one request whose body is neither JSON nor read by its
// handler.  The web server streams it to eyeSlotBody() while it parses the
// request -- before guarded<> has run -- so that callback makes the
// credential check itself, silently, before a byte reaches flash.  The
// handler that follows is guarded as usual; it delivers any refusal, and
// otherwise reports what the stream came to.

static void getEyeSlot(void) {
  EyeSlotState e;
  stateEyeSlot(e);
  JsonDocument d;
  fillEyeSlot(d.to<JsonObject>(), e);
  sendJson(200, d);
}

// What the streamed body came to, for putEyeSlot() to report.  `streamed`
// tells "not an eye file" apart from "no body reached us at all", which is
// what a multipart form looks like: the server parses those itself and
// never calls the raw callback.
static bool slotStreamed = false;
static bool slotAllowed = false;
static EyeLoadResult slotResult = EYE_LOAD_INCOMPLETE;

// The library reads a streamed body HTTP_RAW_BUFLEN bytes at a time, on the
// render loop, waiting for each byte up to the client's read timeout.  Two
// things followed.  The eye file is 158400 bytes, 440 past a whole number of
// 1436-byte reads, so the last read asked for more than was left and waited
// out the timeout: five seconds of frozen eyes at the end of every upload,
// ten when a digest login meant sending it twice.  And a sender trickling a
// byte at a time kept every wait alive, so a deadline checked between reads
// was never reached.
//
// So the bytes each read needs are waited for here, before it, a
// millisecond at a time against a deadline, and the reads are told not to
// wait: they find their bytes already there.  Past the deadline the request
// is abandoned.  A refused upload is still read to the end -- its refusal
// can only be sent once the body is gone, and a browser whose login went
// stale sends the file again -- but it gets less time.  A real upload over
// a LAN takes under a second.
#define EYE_UPLOAD_MAX_MS 30000
#define EYE_REFUSED_MAX_MS 10000
static uint32_t slotStartedMs = 0;

// False if the request was abandoned, in which case the library's next read
// comes back empty and the stream ends RAW_ABORTED.
static bool awaitNextRead(void) {
  const HTTPRaw &r = srv->raw();
  const size_t left = srv->clientContentLength() - r.totalSize;
  const size_t want = left < HTTP_RAW_BUFLEN ? left : HTTP_RAW_BUFLEN;
  const uint32_t limit = slotAllowed ? EYE_UPLOAD_MAX_MS : EYE_REFUSED_MAX_MS;
  while (want && (size_t)srv->pendingBytes() < want) {
    if (millis() - slotStartedMs > limit || !srv->clientConnected()) {
      srv->abortRequest();
      return false;
    }
    delay(1);
  }
  return true;
}

static void eyeSlotBody(void) {
  HTTPRaw &r = srv->raw();
  switch (r.status) {
  case RAW_START:
    slotStartedMs = millis();
    slotStreamed = true;
    slotAllowed = authPermits(*srv);
    slotResult = EYE_LOAD_INCOMPLETE;
    if (slotAllowed)
      stateEyeLoadBegin(srv->clientContentLength());
    srv->setReadTimeoutMs(1); // every read finds its bytes waiting
    awaitNextRead();
    break;
  case RAW_WRITE:
    if (slotAllowed)
      stateEyeLoadChunk(r.buf, r.currentSize);
    awaitNextRead();
    break;
  case RAW_END:
    if (slotAllowed)
      slotResult = stateEyeLoadEnd();
    break;
  case RAW_ABORTED:
    if (slotAllowed)
      stateEyeLoadAbort();
    slotStreamed = false; // no handler follows an aborted request
    break;
  }
}

static void putEyeSlot(void) {
  const bool streamed = slotStreamed;
  slotStreamed = false; // consumed: the next request starts clean
  if (!streamed) {
    sendError(400, "expected the eye file as the request body, with "
                   "Content-Type: application/octet-stream");
    return;
  }
  switch (slotResult) {
  case EYE_LOAD_OK:
    getEyeSlot();
    return;
  case EYE_LOAD_NO_SLOT:
  case EYE_LOAD_NAME_TAKEN:
    sendError(409, eyeLoadResultText(slotResult));
    return;
  case EYE_LOAD_FLASH:
    sendError(500, eyeLoadResultText(slotResult));
    return;
  default: // everything else is something wrong with the file
    sendError(400, eyeLoadResultText(slotResult));
    return;
  }
}

static void deleteEyeSlot(void) {
  if (!stateEyeUnload()) {
    EyeSlotState e;
    stateEyeSlot(e);
    if (e.available)
      sendError(500, "erasing the eye slot failed");
    else
      sendError(409, eyeLoadResultText(EYE_LOAD_NO_SLOT));
    return;
  }
  getEyeSlot();
}

static void getEye(void) {
  DeviceState s;
  stateGet(s);
  JsonDocument d;
  fillEye(d.to<JsonObject>(), s);
  sendJson(200, d);
}

static void putEye(void) {
  JsonDocument b;
  if (!readBody(b))
    return;
  if (wrongType<bool>(b, "next", "true or false") ||
      wrongType<const char *>(b, "name", "a string") ||
      wrongType<long>(b, "index", "a whole number"))
    return;
  if (b["next"].as<bool>()) {
    stateEyeNext();
  } else if (b["name"].is<const char *>()) {
    if (!stateEyeSetName(b["name"])) {
      sendError(404, "no such eye design on this board");
      return;
    }
  } else if (b["index"].is<long>()) {
    if (!stateEyeSetIndex(b["index"].as<long>())) {
      sendError(404, "index out of range");
      return;
    }
  } else {
    sendError(400, "expected name, index or next");
    return;
  }
  getEye();
}

static void getGaze(void) {
  DeviceState s;
  stateGet(s);
  JsonDocument d;
  fillGaze(d.to<JsonObject>(), s);
  sendJson(200, d);
}

static void putGaze(void) {
  JsonDocument b;
  if (!readBody(b))
    return;
  if (wrongType<const char *>(b, "mode", "\"auto\"") ||
      wrongType<long>(b, "x", "a whole number") ||
      wrongType<long>(b, "y", "a whole number"))
    return;
  const char *mode = b["mode"] | "";
  if (!strcmp(mode, "auto")) {
    stateGazeAuto();
  } else if (b["x"].is<long>() && b["y"].is<long>()) {
    if (!stateGazeSet(b["x"].as<long>(), b["y"].as<long>())) {
      sendError(400, "x and y must each be 0-1023");
      return;
    }
  } else {
    sendError(400, "expected x and y, or mode=auto");
    return;
  }
  getGaze();
}

static void getDilate(void) {
  DeviceState s;
  stateGet(s);
  JsonDocument d;
  fillDilate(d.to<JsonObject>(), s);
  sendJson(200, d);
}

static void putDilate(void) {
  JsonDocument b;
  if (!readBody(b))
    return;
  if (wrongType<const char *>(b, "mode", "\"auto\"") ||
      wrongType<long>(b, "percent", "a whole number"))
    return;
  const char *mode = b["mode"] | "";
  if (!strcmp(mode, "auto")) {
    stateDilationAuto();
  } else if (b["percent"].is<long>()) {
    if (!stateDilationSet(b["percent"].as<long>())) {
      sendError(400, "percent must be 0-100");
      return;
    }
  } else {
    sendError(400, "expected percent, or mode=auto");
    return;
  }
  getDilate();
}

static void getPupil(void) {
  DeviceState s;
  stateGet(s);
  JsonDocument d;
  d["on"] = s.pupilOn;
  sendJson(200, d);
}

static void putPupil(void) {
  JsonDocument b;
  if (!readBody(b))
    return;
  if (!b["on"].is<bool>()) {
    sendError(400, "expected on: true or false");
    return;
  }
  statePupilSet(b["on"]);
  getPupil();
}

static void getSwap(void) {
  DeviceState s;
  stateGet(s);
  JsonDocument d;
  d["on"] = s.swapped;
  sendJson(200, d);
}

static void getFlip(void) {
  DeviceState s;
  stateGet(s);
  JsonDocument d;
  fillFlip(d.to<JsonObject>(), s);
  sendJson(200, d);
}

// Either side, or both.  The names are the viewer's, like the "YOUR LEFT"
// line of the splash, since the viewer is who can see which one is upside
// down.
static void putFlip(void) {
  JsonDocument b;
  if (!readBody(b))
    return;
  static const char *const sides[2] = {"left", "right"};
  bool any = false;
  for (uint8_t e = 0; e < 2; e++) {
    if (wrongType<bool>(b, sides[e], "true or false"))
      return;
    any |= b[sides[e]].is<bool>();
  }
  if (!any) {
    sendError(400, "expected left and/or right: true or false");
    return;
  }
  // Checked, above, before either side changed; a one-panel build is the
  // only way the second can still be refused, and it is refused whole.
  if (b["right"].is<bool>() && displayCount() < 2) {
    sendError(400, "no such panel in this build");
    return;
  }
  for (uint8_t e = 0; e < 2; e++)
    if (b[sides[e]].is<bool>())
      stateFlipSet(e, b[sides[e]]);
  getFlip();
}

static void getDim(void) {
  DeviceState s;
  stateGet(s);
  JsonDocument d;
  fillDim(d.to<JsonObject>(), s);
  sendJson(200, d);
}

// Any of percent, gamma, trim.left, trim.right and sweep.  All checked
// before any is applied.
static void putDim(void) {
  JsonDocument b;
  if (!readBody(b))
    return;
  if (wrongType<long>(b, "percent", "a whole number, 0-100") ||
      wrongType<float>(b, "gamma", "a number, 1.0-4.0") ||
      wrongType<JsonObjectConst>(b, "trim", "an object") ||
      wrongType<bool>(b, "sweep", "true or false"))
    return;

  const bool havePercent = b["percent"].is<long>();
  const long percent = b["percent"].as<long>();
  if (havePercent && (percent < 0 || percent > 100)) {
    sendError(400, "percent must be 0-100");
    return;
  }
  const bool haveGamma = b["gamma"].is<float>();
  const long gammaX10 = lroundf(b["gamma"].as<float>() * 10.0f);
  if (haveGamma && (gammaX10 < DIM_GAMMA_MIN || gammaX10 > DIM_GAMMA_MAX)) {
    sendError(400, "gamma must be 1.0-4.0");
    return;
  }
  static const char *const sides[2] = {"left", "right"};
  JsonObjectConst trim = b["trim"];
  bool haveTrim[2] = {false, false};
  long trimValue[2] = {0, 0};
  for (uint8_t e = 0; e < 2; e++) {
    JsonVariantConst v = trim[sides[e]];
    if (v.isNull())
      continue;
    if (!v.is<long>() || v.as<long>() < -50 || v.as<long>() > 50) {
      sendError(400, "trim.left and trim.right must be whole numbers, -50 to 50");
      return;
    }
    if (e >= displayCount()) {
      sendError(400, "no such panel in this build");
      return;
    }
    haveTrim[e] = true;
    trimValue[e] = v.as<long>();
  }
  if (!havePercent && !haveGamma && !haveTrim[0] && !haveTrim[1] &&
      !b["sweep"].is<bool>()) {
    sendError(400, "expected percent, gamma, trim or sweep");
    return;
  }

  if (haveGamma)
    stateDimSetGamma(gammaX10);
  for (uint8_t e = 0; e < 2; e++)
    if (haveTrim[e])
      stateDimSetTrim(e, trimValue[e]);
  if (havePercent)
    stateDimSet(percent); // ends a sweep, so before one is started
  if (b["sweep"].is<bool>())
    stateDimSweep(b["sweep"]);
  getDim();
}

static void putSwap(void) {
  JsonDocument b;
  if (!readBody(b))
    return;
  if (!b["on"].is<bool>()) {
    sendError(400, "expected on: true or false");
    return;
  }
  stateSwapSet(b["on"]);
  getSwap();
}

static void getCpu(void) {
  DeviceState s;
  stateGet(s);
  JsonDocument d;
  d["mhz"] = s.cpuMhz;         // running now
  d["setting"] = s.cpuSetting; // from the next restart
  sendJson(200, d);
}

// Stored at once; takes effect at the next restart -- see stateCpuSet().
static void putCpu(void) {
  JsonDocument b;
  if (!readBody(b))
    return;
  const long mhz = b["mhz"].is<long>() ? b["mhz"].as<long>() : 0;
  if (mhz != 160 && mhz != 240) {
    sendError(400, "expected mhz: 160 or 240");
    return;
  }
  if (!stateCpuSet(mhz)) {
    sendError(500, "the setting could not be stored");
    return;
  }
  getCpu();
}

static void getClock(void) {
  DeviceState s;
  stateGet(s);
  JsonDocument d;
  fillClock(d.to<JsonObject>(), s);
  sendJson(200, d);
}

// Every field is optional; whatever is present is applied -- but only once
// all of it has been checked, so a bad colour does not leave a new rate
// behind it.
static void putClock(void) {
  JsonDocument b;
  if (!readBody(b))
    return;
#if !CLOCK
  // Every setter below refuses without the clock, and their results are not
  // checked -- everything is validated before anything is applied -- so say
  // so here rather than reply as though the change had been made.
  sendError(404, "this firmware was built without the clock face");
  return;
#endif
  if (wrongType<bool>(b, "on", "true or false") ||
      wrongType<bool>(b, "seconds", "true or false") ||
      wrongType<long>(b, "rate", "a whole number") ||
      wrongType<const char *>(b, "time", "\"HH:MM\" or \"HH:MM:SS\"") ||
      wrongType<JsonObjectConst>(b, "colors", "an object"))
    return;

  long rate = 0;
  bool haveRate = b["rate"].is<long>();
  if (haveRate) {
    rate = b["rate"].as<long>();
    if (rate < 1 || rate > 3600) {
      sendError(400, "rate must be 1-3600");
      return;
    }
  }
  uint8_t h = 0, m = 0, sec = 0;
  bool haveTime = b["time"].is<const char *>();
  if (haveTime && !parseTimeOfDay(b["time"], true, h, m, sec)) {
    sendError(400, "time must be \"HH:MM\" or \"HH:MM:SS\"");
    return;
  }
  static const char *const names[3] = {"hour", "minute", "second"};
  uint32_t rgb[3];
  bool haveColor[3] = {false, false, false};
  JsonObjectConst c = b["colors"];
  for (uint8_t i = 0; i < 3; i++) {
    JsonVariantConst v = c[names[i]];
    if (v.isNull())
      continue;
    if (!v.is<const char *>() || !parseHexColor(v.as<const char *>(), rgb[i])) {
      sendError(400, "colours must be six hex digits, e.g. FF8800");
      return;
    }
    haveColor[i] = true;
  }

  if (b["on"].is<bool>())
    stateClockSetOn(b["on"]);
  if (b["seconds"].is<bool>())
    stateClockSetSeconds(b["seconds"]);
  if (haveRate)
    stateClockSetRate(rate);
  if (haveTime)
    stateClockSetTime(h, m, sec);
  for (uint8_t i = 0; i < 3; i++)
    if (haveColor[i])
      stateClockSetColor(i, rgb[i]);
  getClock();
}

static void getNet(void) {
  JsonDocument d;
  fillNet(d.to<JsonObject>());
  sendJson(200, d);
}

// Whether the address cards are up on the panels.  Modelled as state rather
// than as a one-shot because it can be dismissed as well as raised -- the
// panels are either showing the eyes or showing the address.
static void getNetInfo(void) {
  JsonDocument d;
  d["on"] = netShowing();
  sendJson(200, d);
}

static void putNetInfo(void) {
  JsonDocument b;
  if (!readBody(b))
    return;
  if (!b["on"].is<bool>()) {
    sendError(400, "expected on: true or false");
    return;
  }
  if (b["on"].as<bool>())
    netShow();
  else
    netHide();
  getNetInfo();
}

static void getTz(void) {
  JsonDocument d;
  d["tz"] = tzString;
  d["synced"] = timeSynced;
  // Name and region only.  The POSIX string was two thirds of this reply and
  // nothing reads it: a client picks a name and sends the name back, and the
  // device resolves it.  At 61 zones that mattered -- this was the biggest
  // response the API had, and the one with the worst tail latency.
  JsonArray a = d["zones"].to<JsonArray>();
  for (uint8_t i = 0; i < numTzChoices; i++) {
    JsonObject o = a.add<JsonObject>();
    o["name"] = tzChoices[i].name;
    o["region"] = tzChoices[i].region;
  }
  sendJson(200, d);
}

static void putTz(void) {
  JsonDocument b;
  if (!readBody(b))
    return;
  if (!b["tz"].is<const char *>()) {
    sendError(400, "expected tz: a name or a POSIX string");
    return;
  }
  if (!stateTzSet(b["tz"])) {
    sendError(400, "not a known zone name or a POSIX timezone string");
    return;
  }
  getTz();
}

// What this firmware is: fixed for the life of a build, so the page reads it
// once at startup rather than dragging it through every poll.
static void getInfo(void) {
  JsonDocument d;
  d["name"] = WIFI_HOSTNAME;
  d["version"] = FIRMWARE_VERSION;
  d["commit"] = FIRMWARE_COMMIT;
  d["dirty"] = (bool)GIT_DIRTY;
  d["project"] = PROJECT_URL;
  d["built"] = __DATE__ " " __TIME__;
  d["arduino"] = ESP.getSdkVersion();
  d["api"] = "v1";
  d["rtc"] = (bool)RTC;
  d["auth"] = authRequired();
  sendJson(200, d);
}

static void getNtp(void) {
  JsonDocument d;
  fillTime(d.to<JsonObject>());
  sendJson(200, d);
}

// Asking again now, rather than waiting out the three hours.  The reply says
// only that the request went out: an answer arrives asynchronously, and the
// page sees it on its next poll as a changed lastSyncSeconds.
static void putNtp(void) {
  JsonDocument b;
  if (!readBody(b))
    return;

  if (wrongType<bool>(b, "enabled", "true or false") ||
      wrongType<const char *>(b, "op", "\"sync\""))
    return;
  if (b["enabled"].is<bool>()) {
    stateNtpSetEnabled(b["enabled"]);
    getNtp();
    return;
  }

  const char *op = b["op"] | "";
  if (strcmp(op, "sync")) {
    sendError(400, "expected enabled, or op=sync");
    return;
  }
  if (!netNtpEnabled()) {
    sendError(409, "the time client is switched off");
    return;
  }
  if (!netNtpSyncNow()) {
    sendError(409, "the time client is not running; there is no link yet");
    return;
  }
  sendOk();
}

#if RTC

// The battery-backed clock.  "valid" is the one worth reading: the registers
// always hold something, and only the oscillator-stop flag says whether it is
// a time anyone should believe.
static void getRtc(void) {
  JsonDocument d;
  d["present"] = rtcPresent();
  d["valid"] = rtcValid();
  time_t utc;
  if (rtcRead(utc)) {
    struct tm g;
    gmtime_r(&utc, &g);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &g);
    d["utc"] = buf;
    d["epoch"] = (uint32_t)utc;
  }
  float c;
  if (rtcTemperature(c))
    d["temperatureC"] = c;
  sendJson(200, d);
}

// Writing means "store what the clock currently says", not "set it to this":
// the time comes from whichever source is in charge, so there is no way for
// a client to put a wrong time in behind NTP's back.
static void putRtc(void) {
  JsonDocument b;
  if (!readBody(b))
    return;
  if (wrongType<const char *>(b, "op", "\"sync\""))
    return;
  const char *op = b["op"] | "";
  if (strcmp(op, "sync")) {
    sendError(400, "op must be sync");
    return;
  }
  if (!rtcPresent()) {
    sendError(404, "no RTC found on the bus");
    return;
  }
  if (!rtcWriteNow()) {
    sendError(409, "the clock has no real time to store yet");
    return;
  }
  getRtc();
}

#endif // RTC

// WiFi.  The password goes in and never comes out.  A build may have no
// authentication at all (the _open environments), and then anything
// readable here is readable by anyone on the network; a stored password does
// not need to be.
static void getWifi(void) {
  JsonDocument d;
  bool up = WiFi.status() == WL_CONNECTED;
  d["state"] = up ? "up" : (netState == NET_PORTAL ? "portal" : "down");
  d["ssid"] = WiFi.SSID(); // the one it is on, "" if none
  char saved[33];
  netStoredSsid(saved, sizeof(saved)); // the one it would try at boot
  d["stored"] = saved;
  if (up)
    d["rssi"] = WiFi.RSSI();
  d["portalName"] = WIFI_AP_NAME;
  d["rebooting"] = netRebootPending();
  sendJson(200, d);
}

// Every branch here ends in a reboot, applied a moment after this response
// goes out.  See the note in net.h for why reconnecting in place is not
// worth the trouble.
static void putWifi(void) {
  JsonDocument b;
  if (!readBody(b))
    return;

  if (wrongType<const char *>(b, "op", "\"forget\" or \"portal\"") ||
      wrongType<const char *>(b, "ssid", "a string") ||
      wrongType<const char *>(b, "pass", "a string"))
    return;
  const char *op = b["op"] | "";
  if (!strcmp(op, "forget")) {
    netRequestForget();
  } else if (!strcmp(op, "portal")) {
    netRequestPortal();
  } else if (b["ssid"].is<const char *>()) {
    if (!netRequestJoin(b["ssid"], b["pass"] | "")) {
      sendError(400, "ssid must be 1-32 characters and pass at most 63, "
                     "neither with control characters");
      return;
    }
  } else {
    sendError(400, "expected ssid, or op=forget or op=portal");
    return;
  }

  // Answered before the radio moves, and deliberately not getWifi(): what
  // this reports is the request, not a state that has taken effect yet.
  JsonDocument d;
  d["ok"] = true;
  d["rebooting"] = true;
  d["note"] = "the board reboots in a moment; it may come back on a "
              "different address";
  sendJson(200, d);
}

// Verbs that are not state: things the device *does* rather than *is*.
static void postAction(void) {
  JsonDocument b;
  if (!readBody(b))
    return;
  if (wrongType<const char *>(b, "action", "a string"))
    return;
  const char *a = b["action"] | "";
  if (!strcmp(a, "blink"))
    stateBlink();
  else if (!strcmp(a, "startle"))
    stateStartle();
  else if (!strcmp(a, "splash"))
    stateSplash();
  else if (!strcmp(a, "netinfo"))
    netShow(); // synonym for PUT /netinfo {"on":true}
  else if (!strcmp(a, "restart"))
    stateRestart(); // a moment after this reply, which is the point
  else {
    sendError(400, "action must be blink, startle, splash, netinfo or restart");
    return;
  }
  sendOk();
}

// ---------------------------------------------------------- credentials --
//
// Compiled in only when there is a credential to change.  Note what is
// missing: no handler here ever returns a password, a token or even a hash.
// The page needs to know which credentials are set and whether each is still
// the one built into the firmware, and that is all it is told.

#if AUTH_HTTP || AUTH_TOKEN || OTA_AUTH

static void getCredentials(void) {
  JsonDocument d;
  // "required" is compile-time: which credentials this firmware enforces.
  // "stored" is runtime: which have been changed from the built-in value.
  JsonObject http = d["http"].to<JsonObject>();
  http["required"] = (bool)AUTH_HTTP;
#if AUTH_HTTP
  http["user"] = credGet(CRED_USER); // a username is not a secret
  http["stored"] = credIsStored(CRED_USER) || credIsStored(CRED_PASS);
#endif

  JsonObject token = d["token"].to<JsonObject>();
  token["required"] = (bool)AUTH_TOKEN;
#if AUTH_TOKEN
  token["stored"] = credIsStored(CRED_TOKEN);
#endif

  JsonObject ota = d["ota"].to<JsonObject>();
  ota["required"] = (bool)OTA_AUTH;
#if OTA_AUTH
  ota["stored"] = credIsStored(CRED_OTA);
#endif

  // Set once an OTA password has changed: ArduinoOTA will not accept a new
  // one until the firmware restarts.
  d["rebootNeeded"] = credRebootPending();
  d["clearText"] = true; // no HTTPS; see credentials.h
  // Whether PUT will be accepted at all -- see putCredentials().
  d["changeable"] = authRequired();
  sendJson(200, d);
}

static void putCredentials(void) {
  // An unauthenticated API must not be able to set credentials.
  //
  // The case that makes this necessary: OTA_PASSWORD set, but AUTH_HTTP and
  // AUTH_TOKEN both off.  The update password is then the only thing standing
  // between the network and flashing whatever firmware it likes -- and if
  // this endpoint were open, anything on the network could simply replace
  // that password with one of its own.  A way to change a credential without
  // presenting one is a back door however politely it is written.
  //
  // So this needs the API itself to require a credential.  The board still
  // works exactly as before without one; it just cannot be reconfigured
  // remotely, which is the correct behaviour for a device that does not know
  // who it is talking to.
  if (!authRequired()) {
    sendError(403, "changing credentials needs the API to require one: build "
                   "with AUTH_HTTP=1 or AUTH_TOKEN=1");
    return;
  }

  JsonDocument b;
  if (!readBody(b))
    return;

#if AUTH_HTTP
  // Prove you know the password you are replacing.  Being authenticated is
  // not quite the same thing: a browser holds digest credentials for the
  // realm and will attach them to whatever asks, so without this a page you
  // merely visited could change the password on a board you are logged into.
  // judge() now refuses requests another site sent, but that rests on what
  // the browser reports, and this is one field.
  const char *current = b["current"] | "";
  if (!credMatches(CRED_PASS, current)) {
    sendError(403, "the current password does not match");
    return;
  }
#endif

  // Collect, then check every value, and only then apply any of them.  A
  // rejected fourth field must not leave the first three written: "rejected"
  // should mean nothing happened.
  struct Change {
    CredKind kind;
    const char *field;
    bool present;
    const char *value;
  } changes[] = {
      {CRED_USER, "user", false, ""},
      {CRED_PASS, "password", false, ""},
      {CRED_TOKEN, "token", false, ""},
      {CRED_OTA, "otaPassword", false, ""},
  };

  int wanted = 0;
  for (auto &c : changes) {
    if (wrongType<const char *>(b, c.field, "a string"))
      return;
    if (!b[c.field].is<const char *>())
      continue;
#if !AUTH_HTTP
    if (c.kind == CRED_USER || c.kind == CRED_PASS) {
      sendError(400, "this firmware was built without AUTH_HTTP");
      return;
    }
#endif
#if !AUTH_TOKEN
    if (c.kind == CRED_TOKEN) {
      sendError(400, "this firmware was built without AUTH_TOKEN");
      return;
    }
#endif
#if !OTA_AUTH
    if (c.kind == CRED_OTA) {
      sendError(400, "this firmware was built without an OTA password");
      return;
    }
#endif
    c.present = true;
    c.value = b[c.field];
    wanted++;
  }

  if (!wanted) {
    sendError(400, "nothing to change: send user, password, token or "
                   "otaPassword");
    return;
  }

  String err;
  for (auto &c : changes) {
    if (c.present && !credCheck(c.kind, c.value, err)) {
      // Whichever field failed, name it -- "must not be empty" is no use if
      // four fields were sent.
      String msg = String(c.field) + ": " + err;
      sendError(400, msg.c_str());
      return; // nothing written yet
    }
  }

  for (auto &c : changes) {
    if (c.present && !credSet(c.kind, c.value, err)) {
      // Checked a moment ago, so this is storage failing rather than the
      // value being wrong -- and by now some of the others may have been
      // written, which the caller needs to know.
      String msg = String(c.field) + ": " + err +
                   " (earlier fields in this request may have been saved)";
      sendError(500, msg.c_str());
      return;
    }
  }

  JsonDocument d;
  d["ok"] = true;
  d["rebootNeeded"] = credRebootPending();
  sendJson(200, d);
}

#endif // AUTH_HTTP || AUTH_TOKEN || OTA_AUTH

// ----------------------------------------------------------------- sleep --
//
// Times go over the wire as "HH:MM" rather than as minute counts: the page's
// <input type=time> produces exactly that, and an API a person can drive from
// curl is worth more here than saving two string parses.

#if SLEEP

static void fillSleep(JsonObject o) {
  char buf[6];
  o["enabled"] = sleepEnabled();
  snprintf(buf, sizeof(buf), "%02u:%02u", sleepStart() / 60, sleepStart() % 60);
  o["start"] = buf;
  snprintf(buf, sizeof(buf), "%02u:%02u", sleepStop() / 60, sleepStop() % 60);
  o["stop"] = buf;
  o["level"] = sleepLevel();
  o["asleep"] = sleepIsAsleep();
  // The board's own local time of day, which is the value the window is
  // judged against.  Reported so that a client never has to reconstruct it
  // from a timezone and a UTC clock and hope it agreed -- and so the page can
  // say what the board thinks the time is when that is the thing in doubt.
  uint32_t sec;
  if (timeLocalSecOfDay(sec)) {
    char n[6];
    snprintf(n, sizeof(n), "%02u:%02u", (unsigned)(sec / 3600),
             (unsigned)((sec / 60) % 60));
    o["now"] = n;
  }
  // Why, not just whether.  "enabled but the board does not know the time" is
  // a different state from "enabled and it is daytime", and a user whose eyes
  // did not go dark needs to be told which one they are in.
  o["reason"] = sleepReason();

  uint16_t mins;
  bool toAsleep;
  if (sleepNextChange(mins, toAsleep)) {
    o["changesInMinutes"] = mins;
    o["changesToAsleep"] = toAsleep;
  }
}

static void getSleep(void) {
  JsonDocument d;
  fillSleep(d.to<JsonObject>());
  sendJson(200, d);
}

// "HH:MM", or a bare minute count for anything driving this by hand.
static bool parseHHMM(const char *s, uint16_t &out) {
  uint8_t h, m, unused;
  long mins;
  if (parseTimeOfDay(s, false, h, m, unused))
    mins = h * 60 + m;
  else if (!parseLong(s, 0, 1439, mins))
    return false;
  out = (uint16_t)mins;
  return true;
}

static void putSleep(void) {
  JsonDocument b;
  if (!readBody(b))
    return;

  // Validate everything before changing anything, so a bad "stop" does not
  // leave a new "start" applied.
  if (wrongType<const char *>(b, "start", "\"HH:MM\"") ||
      wrongType<const char *>(b, "stop", "\"HH:MM\"") ||
      wrongType<long>(b, "level", "a whole number") ||
      wrongType<bool>(b, "enabled", "true or false"))
    return;
  uint16_t start = sleepStart(), stop = sleepStop();
  bool haveWindow = false;

  if (b["start"].is<const char *>()) {
    if (!parseHHMM(b["start"], start)) {
      sendError(400, "start must be \"HH:MM\"");
      return;
    }
    haveWindow = true;
  }
  if (b["stop"].is<const char *>()) {
    if (!parseHHMM(b["stop"], stop)) {
      sendError(400, "stop must be \"HH:MM\"");
      return;
    }
    haveWindow = true;
  }

  uint8_t level = sleepLevel();
  bool haveLevel = false;
  if (b["level"].is<long>()) {
    long v = b["level"];
    if (v < 0 || v > 100) {
      sendError(400, "level must be 0-100");
      return;
    }
    level = (uint8_t)v;
    haveLevel = true;
  }

  bool haveEnabled = b["enabled"].is<bool>();
  if (!haveWindow && !haveLevel && !haveEnabled) {
    sendError(400, "expected enabled, start, stop or level");
    return;
  }

  SleepChange c; // which also drops any hold keeping the eyes up
  c.setWindow = haveWindow;
  c.start = start;
  c.stop = stop;
  c.setLevel = haveLevel;
  c.level = level;
  c.setEnabled = haveEnabled;
  c.enabled = haveEnabled && b["enabled"].as<bool>();
  stateSleepSet(c);

  JsonDocument d;
  fillSleep(d.to<JsonObject>());
  sendJson(200, d);
}

#endif // SLEEP

static void postSettings(void) {
  JsonDocument b;
  if (!readBody(b))
    return;
  if (wrongType<const char *>(b, "op", "\"save\" or \"forget\""))
    return;
  const char *op = b["op"] | "";
  if (!strcmp(op, "save"))
    stateSave();
  else if (!strcmp(op, "forget"))
    stateForget();
  else {
    sendError(400, "op must be save or forget");
    return;
  }
  sendOk();
}

// ------------------------------------------------------------ registration --


// Read-only and mutable routes are registered separately so a wrong method
// gets a 405 from the framework rather than a confusing 404.
void apiRegister(AuthWebServer &s) {
  srv = &s;

#if AUTH_HTTP || AUTH_TOKEN || OTA_AUTH
  s.on(API "/credentials", HTTP_GET, guarded<getCredentials>);
  s.on(API "/credentials", HTTP_PUT, guarded<putCredentials>);
  s.on(API "/credentials", HTTP_OPTIONS, handleOptions);
  s.on(API "/credentials", HTTP_ANY, notAllowed);
#endif

#if SLEEP
  s.on(API "/sleep", HTTP_GET, guarded<getSleep>);
  s.on(API "/sleep", HTTP_PUT, guarded<putSleep>);
  s.on(API "/sleep", HTTP_OPTIONS, handleOptions);
  s.on(API "/sleep", HTTP_ANY, notAllowed);
#endif

  s.on(API "/state", HTTP_GET, guarded<getState>);
  s.on(API "/state", HTTP_ANY, notAllowed);
  s.on(API "/eyes", HTTP_GET, guarded<getEyes>);
  s.on(API "/eyes", HTTP_ANY, notAllowed);
  s.on(API "/eyes/slot", HTTP_GET, guarded<getEyeSlot>);
  // The fourth argument makes the body stream to eyeSlotBody() instead of
  // being buffered -- 158 KB would not fit -- and it runs before the guard.
  // eyeSlotBody() checks credentials itself for that reason.
  s.on(API "/eyes/slot", HTTP_PUT, guarded<putEyeSlot>, eyeSlotBody);
  s.on(API "/eyes/slot", HTTP_DELETE, guarded<deleteEyeSlot>);
  s.on(API "/eyes/slot", HTTP_OPTIONS, handleOptions);
  s.on(API "/eyes/slot", HTTP_ANY, notAllowed);
  s.on(API "/net", HTTP_GET, guarded<getNet>);
  s.on(API "/net", HTTP_ANY, notAllowed);

  s.on(API "/eye", HTTP_GET, guarded<getEye>);
  s.on(API "/eye", HTTP_PUT, guarded<putEye>);
  s.on(API "/eye", HTTP_OPTIONS, handleOptions);
  s.on(API "/eye", HTTP_ANY, notAllowed);

  s.on(API "/gaze", HTTP_GET, guarded<getGaze>);
  s.on(API "/gaze", HTTP_PUT, guarded<putGaze>);
  s.on(API "/gaze", HTTP_OPTIONS, handleOptions);
  s.on(API "/gaze", HTTP_ANY, notAllowed);

  s.on(API "/dilate", HTTP_GET, guarded<getDilate>);
  s.on(API "/dilate", HTTP_PUT, guarded<putDilate>);
  s.on(API "/dilate", HTTP_OPTIONS, handleOptions);
  s.on(API "/dilate", HTTP_ANY, notAllowed);

  s.on(API "/pupil", HTTP_GET, guarded<getPupil>);
  s.on(API "/pupil", HTTP_PUT, guarded<putPupil>);
  s.on(API "/pupil", HTTP_OPTIONS, handleOptions);
  s.on(API "/pupil", HTTP_ANY, notAllowed);

  s.on(API "/swap", HTTP_GET, guarded<getSwap>);
  s.on(API "/swap", HTTP_PUT, guarded<putSwap>);
  s.on(API "/swap", HTTP_OPTIONS, handleOptions);
  s.on(API "/swap", HTTP_ANY, notAllowed);
  s.on(API "/cpu", HTTP_GET, guarded<getCpu>);
  s.on(API "/cpu", HTTP_PUT, guarded<putCpu>);
  s.on(API "/cpu", HTTP_OPTIONS, handleOptions);
  s.on(API "/cpu", HTTP_ANY, notAllowed);
  s.on(API "/dim", HTTP_GET, guarded<getDim>);
  s.on(API "/dim", HTTP_PUT, guarded<putDim>);
  s.on(API "/dim", HTTP_OPTIONS, handleOptions);
  s.on(API "/dim", HTTP_ANY, notAllowed);
  s.on(API "/flip", HTTP_GET, guarded<getFlip>);
  s.on(API "/flip", HTTP_PUT, guarded<putFlip>);
  s.on(API "/flip", HTTP_OPTIONS, handleOptions);
  s.on(API "/flip", HTTP_ANY, notAllowed);

  s.on(API "/clock", HTTP_GET, guarded<getClock>);
  s.on(API "/clock", HTTP_PUT, guarded<putClock>);
  s.on(API "/clock", HTTP_OPTIONS, handleOptions);
  s.on(API "/clock", HTTP_ANY, notAllowed);

  s.on(API "/ntp", HTTP_GET, guarded<getNtp>);
  s.on(API "/ntp", HTTP_PUT, guarded<putNtp>);
  s.on(API "/ntp", HTTP_OPTIONS, handleOptions);
  s.on(API "/ntp", HTTP_ANY, notAllowed);

#if RTC
  s.on(API "/rtc", HTTP_GET, guarded<getRtc>);
  s.on(API "/rtc", HTTP_PUT, guarded<putRtc>);
  s.on(API "/rtc", HTTP_OPTIONS, handleOptions);
  s.on(API "/rtc", HTTP_ANY, notAllowed);
#endif

  s.on(API "/info", HTTP_GET, guarded<getInfo>);
  s.on(API "/info", HTTP_ANY, notAllowed);

  s.on(API "/wifi", HTTP_GET, guarded<getWifi>);
  s.on(API "/wifi", HTTP_PUT, guarded<putWifi>);
  s.on(API "/wifi", HTTP_OPTIONS, handleOptions);
  s.on(API "/wifi", HTTP_ANY, notAllowed);

  s.on(API "/netinfo", HTTP_GET, guarded<getNetInfo>);
  s.on(API "/netinfo", HTTP_PUT, guarded<putNetInfo>);
  s.on(API "/netinfo", HTTP_OPTIONS, handleOptions);
  s.on(API "/netinfo", HTTP_ANY, notAllowed);

  s.on(API "/tz", HTTP_GET, guarded<getTz>);
  s.on(API "/tz", HTTP_PUT, guarded<putTz>);
  s.on(API "/tz", HTTP_OPTIONS, handleOptions);
  s.on(API "/tz", HTTP_ANY, notAllowed);

  s.on(API "/action", HTTP_POST, guarded<postAction>);
  s.on(API "/action", HTTP_OPTIONS, handleOptions);
  s.on(API "/action", HTTP_ANY, notAllowed);

  s.on(API "/settings", HTTP_POST, guarded<postSettings>);
  s.on(API "/settings", HTTP_OPTIONS, handleOptions);
  s.on(API "/settings", HTTP_ANY, notAllowed);
}

#endif // NETWORK
