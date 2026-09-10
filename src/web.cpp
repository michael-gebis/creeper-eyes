// The web interface and over-the-air updates.  Both are thin: /cmd hands
// straight to the console's dispatcher, so every command works over HTTP the
// moment it is added and there is no second implementation to keep in step.

#include "config.h"

#if NETWORK

#include "console.h"
#include "display.h"
#include "net.h"
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

// WEB SERVER ---------------------------------------------------------------
// Deliberately thin.  /cmd feeds the same dispatcher the serial console uses,
// so every command is available over HTTP the moment it is added, and there
// is no second implementation to keep in step.
//
// handleClient() is polled from frame(), not loop(): loop() spends ~10 s
// inside split() per iteration, so a request handled there would sit unserved
// for up to ten seconds.  The cost is that writing a response blocks
// rendering, which is why the pages are kept small.

static WebServer server(80);

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

void webHandleRoot(void) {
  StringPrint st, nt;
  cmdStatus(st);
  netReport(nt);

  String h;
  h.reserve(2048);
  h += F("<!doctype html><meta name=viewport content='width=device-width,"
         "initial-scale=1'><title>frank</title><style>"
         "body{font:14px system-ui;margin:0;padding:16px;background:#14161a;"
         "color:#e6e8eb}h1{font-size:20px;margin:0 0 12px}"
         "pre{background:#1d2026;padding:10px;border-radius:6px;overflow-x:auto}"
         "a,button{display:inline-block;margin:2px;padding:6px 10px;"
         "background:#2a2f38;color:#e6e8eb;border:0;border-radius:5px;"
         "text-decoration:none;cursor:pointer}"
         "form{margin:12px 0}input{padding:6px;width:60%;background:#1d2026;"
         "color:#e6e8eb;border:1px solid #2a2f38;border-radius:5px}</style>"
         "<h1>frank</h1><pre>");
  h += st.buf;
  h += nt.buf;
  h += F("</pre>"
         "<div>"
         "<a href='/cmd?c=eye+next'>next eye</a>"
         "<a href='/cmd?c=blink'>blink</a>"
         "<a href='/cmd?c=startle'>startle</a>"
         "<a href='/cmd?c=clock+on'>clock on</a>"
         "<a href='/cmd?c=clock+off'>clock off</a>"
         "<a href='/cmd?c=pupil'>toggle pupil</a>"
         "<a href='/cmd?c=net'>show address</a>"
         "<a href='/cmd?c=save'>save</a>"
         "</div><div>");
  h += F("<b style='opacity:.6'>timezone:</b> ");
  for (uint8_t i = 0; i < numTzChoices; i++) {
    h += "<a href='/cmd?c=tz+";
    h += tzChoices[i].name;
    h += "'>";
    h += tzChoices[i].name;
    h += "</a>";
  }
  h += F("</div>"
         "<form action='/cmd'><input name='c' placeholder='any console command, "
         "e.g. look 200 800' autofocus><button>run</button></form>"
         "<p style='opacity:.6'>Every serial command works here. "
         "<a href='/cmd?c=help'>help</a></p>");
  server.send(200, "text/html", h);
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

void webBegin(void) {
  server.on("/", webHandleRoot);
  server.on("/cmd", webHandleCmd);
  server.onNotFound([]() { server.send(404, "text/plain", "not found" "\n"); });
  server.begin();
  MDNS.addService("http", "tcp", 80);
  otaBegin();
  DEBUG_PRINTF("[net] web server on http://%s.local/" "\n", WIFI_HOSTNAME);
}

// Service one HTTP request and any OTA traffic.  Called once per rendered
// frame rather than from loop(), which spends ten seconds at a time inside
// split() and would leave requests unanswered for that long.
void webPoll(void) {
  if (netState != NET_UP)
    return;
  server.handleClient();
  ArduinoOTA.handle();
}

#endif // NETWORK
