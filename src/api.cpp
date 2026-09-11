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

#include "display.h"
#include "net.h"
#include "rtc.h"
#include "timekeeping.h"
#include "state.h"
#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>

// The server is owned by web.cpp; this module only hangs routes off it.
static WebServer *S = nullptr;

// --------------------------------------------------------------- plumbing --

// Browsers refuse cross-origin requests without these, which would stop a
// page served from anywhere else driving the device.
static void corsHeaders(void) {
  S->sendHeader("Access-Control-Allow-Origin", "*");
  S->sendHeader("Access-Control-Allow-Methods", "GET, PUT, POST, OPTIONS");
  S->sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

static void sendJson(int code, JsonDocument &doc) {
  String body;
  serializeJson(doc, body);
  corsHeaders();
  S->send(code, "application/json", body);
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
  if (!S->hasArg("plain")) {
    sendError(400, "expected a JSON body with Content-Type: application/json");
    return false;
  }
  DeserializationError e = deserializeJson(doc, S->arg("plain"));
  if (e) {
    sendError(400, "malformed JSON");
    return false;
  }
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
  S->send(204);
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

static void getState(void) {
  DeviceState s;
  stateGet(s);
  JsonDocument d;
  fillEye(d["eye"].to<JsonObject>(), s);
  fillGaze(d["gaze"].to<JsonObject>(), s);
  fillDilate(d["dilate"].to<JsonObject>(), s);
  d["pupil"]["on"] = s.pupilOn;
  d["swap"]["on"] = s.swapped;
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
  sys["fps"] = s.fps;
  sys["freeHeap"] = s.freeHeap;
  sys["uptimeSeconds"] = s.uptimeSec;
  sys["settingsDirty"] = s.settingsDirty;
  sendJson(200, d);
}

static void getEyes(void) {
  DeviceState s;
  stateGet(s);
  JsonDocument d;
  JsonArray a = d["designs"].to<JsonArray>();
  for (uint8_t i = 0; i < stateEyeCount(); i++) {
    JsonObject o = a.add<JsonObject>();
    o["index"] = i;
    o["name"] = stateEyeName(i);
    o["current"] = (i == s.eyeIndex);
  }
  sendJson(200, d);
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
  if (b["next"].is<bool>() && b["next"].as<bool>()) {
    stateNextEye();
  } else if (b["name"].is<const char *>()) {
    if (!stateSetEyeName(b["name"])) {
      sendError(404, "no such eye design in this build");
      return;
    }
  } else if (b["index"].is<int>()) {
    int i = b["index"];
    if (i < 0 || !stateSetEyeIndex((uint8_t)i)) {
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
  const char *mode = b["mode"] | "";
  if (!strcmp(mode, "auto")) {
    stateGazeAuto();
  } else if (b["x"].is<int>() && b["y"].is<int>()) {
    if (!stateSetGaze(b["x"], b["y"])) {
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
  const char *mode = b["mode"] | "";
  if (!strcmp(mode, "auto")) {
    stateDilationAuto();
  } else if (b["percent"].is<int>()) {
    int p = b["percent"];
    if (p < 0 || !stateSetDilation((uint8_t)p)) {
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
  stateSetPupil(b["on"]);
  getPupil();
}

static void getSwap(void) {
  DeviceState s;
  stateGet(s);
  JsonDocument d;
  d["on"] = s.swapped;
  sendJson(200, d);
}

static void putSwap(void) {
  JsonDocument b;
  if (!readBody(b))
    return;
  if (!b["on"].is<bool>()) {
    sendError(400, "expected on: true or false");
    return;
  }
  stateSetSwap(b["on"]);
  getSwap();
}

static void getClock(void) {
  DeviceState s;
  stateGet(s);
  JsonDocument d;
  fillClock(d.to<JsonObject>(), s);
  sendJson(200, d);
}

// Every field is optional; whatever is present is applied.
static void putClock(void) {
  JsonDocument b;
  if (!readBody(b))
    return;

  if (b["on"].is<bool>())
    stateClockSetOn(b["on"]);
  if (b["seconds"].is<bool>())
    stateClockSetSeconds(b["seconds"]);
  if (b["rate"].is<int>() && !stateClockSetRate(b["rate"])) {
    sendError(400, "rate must be 1-3600");
    return;
  }
  if (b["time"].is<const char *>()) {
    unsigned h = 0, m = 0, sec = 0;
    if (sscanf(b["time"], "%u:%u:%u", &h, &m, &sec) < 2 ||
        !stateClockSetTime((uint8_t)h, (uint8_t)m, (uint8_t)sec)) {
      sendError(400, "time must be HH:MM or HH:MM:SS");
      return;
    }
  }
  JsonObject c = b["colors"];
  if (!c.isNull()) {
    static const char *const names[3] = {"hour", "minute", "second"};
    for (int8_t i = 0; i < 3; i++) {
      if (!c[names[i]].is<const char *>())
        continue;
      char *end = nullptr;
      unsigned long v = strtoul(c[names[i]], &end, 16);
      if (!end || *end || v > 0xFFFFFFUL) {
        sendError(400, "colours must be six hex digits, e.g. FF8800");
        return;
      }
      stateClockSetColor(i, (uint32_t)v);
    }
  }
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
  JsonArray a = d["zones"].to<JsonArray>();
  for (uint8_t i = 0; i < numTzChoices; i++) {
    JsonObject o = a.add<JsonObject>();
    o["name"] = tzChoices[i].name;
    o["region"] = tzChoices[i].region;
    o["tz"] = tzChoices[i].posix;
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
  if (!timeSetTz(b["tz"])) {
    sendError(400, "timezone string too long");
    return;
  }
  netStartTime(); // re-apply and re-sync, so a DST change lands at once
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

  if (b["enabled"].is<bool>()) {
    netNtpSetEnabled(b["enabled"]);
    stateMarkDirty(); // it is a saved setting like the timezone
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

// WiFi.  The password goes in and never comes out -- there is no
// authentication on this API, so anything readable here is readable by
// anyone on the network, and a stored password does not need to be.
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

  const char *op = b["op"] | "";
  if (!strcmp(op, "forget")) {
    netRequestForget();
  } else if (!strcmp(op, "portal")) {
    netRequestPortal();
  } else if (b["ssid"].is<const char *>()) {
    if (!netRequestJoin(b["ssid"], b["pass"] | "")) {
      sendError(400, "ssid must be 1-32 characters and pass at most 63");
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
  const char *a = b["action"] | "";
  if (!strcmp(a, "blink"))
    stateBlink();
  else if (!strcmp(a, "startle"))
    stateStartle();
  else if (!strcmp(a, "splash"))
    stateSplash();
  else if (!strcmp(a, "netinfo"))
    netShow(); // synonym for PUT /netinfo {"on":true}
  else {
    sendError(400, "action must be blink, startle, splash or netinfo");
    return;
  }
  sendOk();
}

static void postSettings(void) {
  JsonDocument b;
  if (!readBody(b))
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

#define API "/api/v1"

// Read-only and mutable routes are registered separately so a wrong method
// gets a 405 from the framework rather than a confusing 404.
void apiRegister(WebServer &s) {
  S = &s;

  s.on(API "/state", HTTP_GET, getState);
  s.on(API "/state", HTTP_ANY, notAllowed);
  s.on(API "/eyes", HTTP_GET, getEyes);
  s.on(API "/eyes", HTTP_ANY, notAllowed);
  s.on(API "/net", HTTP_GET, getNet);
  s.on(API "/net", HTTP_ANY, notAllowed);

  s.on(API "/eye", HTTP_GET, getEye);
  s.on(API "/eye", HTTP_PUT, putEye);
  s.on(API "/eye", HTTP_OPTIONS, handleOptions);
  s.on(API "/eye", HTTP_ANY, notAllowed);

  s.on(API "/gaze", HTTP_GET, getGaze);
  s.on(API "/gaze", HTTP_PUT, putGaze);
  s.on(API "/gaze", HTTP_OPTIONS, handleOptions);
  s.on(API "/gaze", HTTP_ANY, notAllowed);

  s.on(API "/dilate", HTTP_GET, getDilate);
  s.on(API "/dilate", HTTP_PUT, putDilate);
  s.on(API "/dilate", HTTP_OPTIONS, handleOptions);
  s.on(API "/dilate", HTTP_ANY, notAllowed);

  s.on(API "/pupil", HTTP_GET, getPupil);
  s.on(API "/pupil", HTTP_PUT, putPupil);
  s.on(API "/pupil", HTTP_OPTIONS, handleOptions);
  s.on(API "/pupil", HTTP_ANY, notAllowed);

  s.on(API "/swap", HTTP_GET, getSwap);
  s.on(API "/swap", HTTP_PUT, putSwap);
  s.on(API "/swap", HTTP_OPTIONS, handleOptions);
  s.on(API "/swap", HTTP_ANY, notAllowed);

  s.on(API "/clock", HTTP_GET, getClock);
  s.on(API "/clock", HTTP_PUT, putClock);
  s.on(API "/clock", HTTP_OPTIONS, handleOptions);
  s.on(API "/clock", HTTP_ANY, notAllowed);

  s.on(API "/ntp", HTTP_GET, getNtp);
  s.on(API "/ntp", HTTP_PUT, putNtp);
  s.on(API "/ntp", HTTP_OPTIONS, handleOptions);
  s.on(API "/ntp", HTTP_ANY, notAllowed);

#if RTC
  s.on(API "/rtc", HTTP_GET, getRtc);
  s.on(API "/rtc", HTTP_PUT, putRtc);
  s.on(API "/rtc", HTTP_OPTIONS, handleOptions);
  s.on(API "/rtc", HTTP_ANY, notAllowed);
#endif

  s.on(API "/info", HTTP_GET, getInfo);
  s.on(API "/info", HTTP_ANY, notAllowed);

  s.on(API "/wifi", HTTP_GET, getWifi);
  s.on(API "/wifi", HTTP_PUT, putWifi);
  s.on(API "/wifi", HTTP_OPTIONS, handleOptions);
  s.on(API "/wifi", HTTP_ANY, notAllowed);

  s.on(API "/netinfo", HTTP_GET, getNetInfo);
  s.on(API "/netinfo", HTTP_PUT, putNetInfo);
  s.on(API "/netinfo", HTTP_OPTIONS, handleOptions);
  s.on(API "/netinfo", HTTP_ANY, notAllowed);

  s.on(API "/tz", HTTP_GET, getTz);
  s.on(API "/tz", HTTP_PUT, putTz);
  s.on(API "/tz", HTTP_OPTIONS, handleOptions);
  s.on(API "/tz", HTTP_ANY, notAllowed);

  s.on(API "/action", HTTP_POST, postAction);
  s.on(API "/action", HTTP_OPTIONS, handleOptions);
  s.on(API "/action", HTTP_ANY, notAllowed);

  s.on(API "/settings", HTTP_POST, postSettings);
  s.on(API "/settings", HTTP_OPTIONS, handleOptions);
  s.on(API "/settings", HTTP_ANY, notAllowed);
}

#endif // NETWORK
