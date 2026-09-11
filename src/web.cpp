// The web server and over-the-air updates.
//
// This module owns the HTTP server and serves the control page; api.cpp
// hangs /api/v1 off the same server, and everything the page shows or does
// goes through that API.  The page itself is a static string in flash, so
// there is no markup here that has to be kept in step with device state.

#include "config.h"

#if NETWORK

#include "console.h"
#include "display.h"
#include "auth.h"
#include "net.h"
#include "page_gz.h" // generated from data/index.html by tools/gen_page.py
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

// WEB SERVER ---------------------------------------------------------------
// handleClient() is polled from frame(), not loop(): loop() spends ~10 s
// inside split() per iteration, so a request handled there would sit unserved
// for up to ten seconds.  The cost is that writing a response blocks
// rendering, which is why responses are kept small and the page polls at a
// leisurely once a second.

// Not static: api.cpp hangs its routes off this one.
WebServer server(80);

#if WEB_CMD_ENDPOINT && COMMANDS

// The escape hatch: hands a line straight to the console's dispatcher, so
// every serial command is reachable over HTTP.  Useful for anything the REST
// API does not model yet, and disabled by setting WEB_CMD_ENDPOINT to 0.

// Collects a command's output so it can be sent as one response.
class StringPrint : public Print {
public:
  String buf;
  size_t write(uint8_t c) override {
    buf += (char)c;
    return 1;
  }
  size_t write(const uint8_t *b, size_t n) override {
    for (size_t i = 0; i < n; i++)
      buf += (char)b[i];
    return n;
  }
};

void webHandleCmd(void) {
  if (!authCheck(server))
    return;
  if (!server.hasArg("c")) {
    server.send(400, "text/plain", "usage: /cmd?c=status" "\n");
    return;
  }
  String c = server.arg("c");
  char line[96];
  strncpy(line, c.c_str(), sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';

  StringPrint out;
  handleCommand(line, out);
  server.send(200, "text/plain", out.buf);
}

#endif // WEB_CMD_ENDPOINT && COMMANDS

// The control page.  Served straight out of flash, gzipped at build time.
//
// It is the same bytes every time, so building it per request would cost RAM
// the renderer wants and put device state back into C++ string concatenation;
// and at 25 KB uncompressed it was roughly one TCP segment per rendered frame,
// which is half a second of page load spent waiting rather than working.
//
// No Accept-Encoding check: every browser made this century sends gzip, and
// the alternative is carrying both copies in flash to serve a client that
// does not exist.  A command-line client wanting to read it uses --compressed.
void webHandleRoot(void) {
  if (!authCheck(server))
    return;
  server.sendHeader("Content-Encoding", "gzip");
  server.send_P(200, "text/html", (PGM_P)PAGE_GZ, PAGE_GZ_LEN);
}

// OVER-THE-AIR UPDATES ------------------------------------------------------
// The reason this is worth having: once the boards are inside a head, the USB
// port is behind however much glue and foam it took to mount them.  Reflashing
// over WiFi is the difference between a tweak and a disassembly.
//
// Progress is reported on the panels because an OTA takes long enough that a
// frozen-looking prop is alarming, and the eyes stop rendering during it --
// ArduinoOTA.handle() runs the transfer to completion once it starts.

void otaBegin(void) {
  ArduinoOTA.setHostname(WIFI_HOSTNAME);
#if OTA_AUTH
  // Without this, anything on the network can flash whatever firmware it
  // likes onto the board -- a larger hole than the API being open, and a
  // cheaper one to close.
  ArduinoOTA.setPassword(OTA_PASSWORD);
#endif

  ArduinoOTA.onStart([]() {
    DEBUG_PRINTF("[ota] update starting" "\n");
    showMessage("UPDATE", "0%", NULL, NULL);
  });

  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    static uint8_t last = 255;
    uint8_t pct = total ? (uint8_t)((done * 100UL) / total) : 0;
    // Redraw only when the number changes: pushing a panel per packet would
    // slow the transfer down considerably.
    if (pct == last)
      return;
    last = pct;
    char buf[8];
    snprintf(buf, sizeof(buf), "%u%%", (unsigned)pct);
    showMessage("UPDATE", buf, NULL, NULL);
  });

  ArduinoOTA.onEnd([]() {
    DEBUG_PRINTF("[ota] done, rebooting" "\n");
    showMessage("UPDATE", "DONE", "rebooting", NULL);
  });

  ArduinoOTA.onError([](ota_error_t e) {
    DEBUG_PRINTF("[ota] failed, error %u" "\n", (unsigned)e);
    showMessage("UPDATE", "FAILED", NULL, NULL);
  });

  ArduinoOTA.begin();
  DEBUG_PRINTF("[net] ota ready: pio run -t upload --upload-port %s.local" "\n",
               WIFI_HOSTNAME);
}

// Called from netOnConnected(), once there is a link to serve over.  The
// order matters: the specific routes and api.cpp's are registered before the
// catch-all, because WebServer matches in registration order.
#if HTTP_TASK

static TaskHandle_t httpTask = NULL;

// Nothing in here may touch a panel.  Handlers that want to paint -- the
// address cards, the splash -- set a flag the render loop picks up.
static void httpTaskLoop(void *arg) {
  (void)arg;
  for (;;) {
    server.handleClient();
    // One tick.  handleClient does not block, so without this the task would
    // spin at the scheduler's expense and starve the idle task on this core,
    // which is what feeds the watchdog.
    vTaskDelay(1);
  }
}

// How much of the stack was never used, in bytes.  Reported so the size above
// can be set from evidence.
uint32_t webTaskStackFree(void) {
  return httpTask ? uxTaskGetStackHighWaterMark(httpTask) * sizeof(StackType_t)
                  : 0;
}

#else
uint32_t webTaskStackFree(void) { return 0; }
#endif // HTTP_TASK

void webBegin(void) {
  authBegin(server);
  server.on("/", webHandleRoot);
#if WEB_CMD_ENDPOINT && COMMANDS
  server.on("/cmd", webHandleCmd);
#endif
  apiRegister(server);
  server.onNotFound([]() { server.send(404, "text/plain", "not found" "\n"); });
  server.begin();
  MDNS.addService("http", "tcp", 80);
  otaBegin();
  DEBUG_PRINTF("[net] web server on http://%s.local/" "\n", WIFI_HOSTNAME);

#if HTTP_TASK
  // After the routes are registered and the socket is listening, so the task
  // cannot serve a half-built server.
  if (!httpTask) {
    xTaskCreatePinnedToCore(httpTaskLoop, "http", HTTP_TASK_STACK, NULL,
                            HTTP_TASK_PRIORITY, &httpTask, HTTP_TASK_CORE);
    DEBUG_PRINTF("[net] http task: core %d, priority %d, %d byte stack" "\n",
                 HTTP_TASK_CORE, HTTP_TASK_PRIORITY, HTTP_TASK_STACK);
  }
#endif
}

// Service one HTTP request and any OTA traffic.  Called once per rendered
// frame rather than from loop(), which spends ten seconds at a time inside
// split() and would leave requests unanswered for that long.
void webPoll(void) {
  if (netState != NET_UP)
    return;

#if HTTP_TASK
  // The server has its own task.  OTA stays here: it paints progress onto the
  // panels, and the panels belong to the render loop.
  ArduinoOTA.handle();
  return;
#endif

  // One call is enough, and draining in a loop was measured to gain nothing.
  // handleClient() falls straight through from accepting a connection into
  // reading, parsing and answering it, so a request never needs a second
  // call to complete -- the "one step of a state machine per call" that the
  // loop was written for does not exist.
  //
  // What remains of the latency is the wait to reach this function again,
  // which is one render frame.  docs/HTTP_LATENCY.md has the measurements
  // and what it would take to go below it.
  server.handleClient();
  ArduinoOTA.handle();
}

#endif // NETWORK
