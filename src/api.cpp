// The REST API.  JSON in, JSON out, over /api/v1.
//
// Every handler is a thin translation: parse a body, call one operation from
// state.h, report what happened.  No device logic lives here, which is what
// keeps it honestly in step with the serial console -- both drive the same
// operations, so neither can grow behaviour the other lacks.
//
// Versioned from the start because this interface has external clients by
// design.  When something has to change incompatibly, /api/v2 can appear
// beside v1 rather than breaking whatever is already talking to it.

#include "config.h"

#if NETWORK

#include "net.h"
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
  if (!S->hasArg("plain")) {
    sendError(400, "expected a JSON body");
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
    o["ipv6"] = WiFi.localIPv6().toString();
    o["gateway"] = WiFi.gatewayIP().toString();
  }
  o["timeSynced"] = timeSynced;
  o["tz"] = tzString;
}

// ---------------------------------------------------------------- handlers --

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
  JsonObject sys = d["system"].to<JsonObject>();
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

static void getTz(void) {
  JsonDocument d;
  d["tz"] = tzString;
  d["synced"] = timeSynced;
  JsonArray a = d["names"].to<JsonArray>();
  for (uint8_t i = 0; i < numTzChoices; i++)
    a.add(tzChoices[i].name);
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
  const char *want = b["tz"];
  const char *named = tzLookup(want); // a shortcut name wins
  if (named)
    want = named;
  if (strlen(want) >= TZ_MAX) {
    sendError(400, "timezone string too long");
    return;
  }
  strncpy(tzString, want, TZ_MAX - 1);
  tzString[TZ_MAX - 1] = '\0';
  netStartTime(); // re-apply and re-sync
  getTz();
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
    netShow();
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
