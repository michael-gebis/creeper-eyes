// WiFi, the setup portal, mDNS and network time.  See net.h.

#include "net.h"

#if NETWORK

#include "display.h"
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiManager.h>


// Defined with the splash helpers, further down.
void showMessage(const char *l1, const char *l2, const char *l3,
                        const char *l4);
void netStartTime(void);  // defined with the time code below
void handleCommand(char *line, Print &out); // the console's dispatcher
void cmdStatus(Print &out);
void netReport(Print &out);
void webBegin(void);      // defined with the web server below
void otaBegin(void);      // defined alongside it

uint8_t netState = NET_DOWN;

// Blocks until connected or the timeout expires.  Returns true on success.
bool wifiWaitConnected(uint32_t ms) {
  uint32_t start = millis();
  while (millis() - start < ms) {
    if (WiFi.status() == WL_CONNECTED)
      return true;
    delay(100);
  }
  return false;
}

// Three sources of credentials, tried in order of how deliberate they are:
//
//   1. whatever the portal last stored, since that was an explicit choice
//      made on this device and is probably the network it is standing in
//   2. the build-time defaults from secrets.ini
//   3. the portal itself
//
// A failure at every stage is not fatal.  The eyes are the point of the
// device; the network is a convenience, so an unreachable one just means
// carrying on offline.
void setupNetwork(void) {
  // Hostname before mode() and begin(), or the DHCP request goes out with
  // the default name and the router records that instead.  Learned the hard
  // way on the wandering-hour-clock.
  WiFi.persistent(true);
  WiFi.setHostname(WIFI_HOSTNAME);
  WiFi.mode(WIFI_STA);

  String savedSsid = WiFi.SSID();
  if (savedSsid.length()) {
    DEBUG_PRINTF("[net] trying stored network '%s'" "\n", savedSsid.c_str());
    WiFi.begin();
    if (wifiWaitConnected(WIFI_CONNECT_MS)) {
      netState = NET_UP;
      return;
    }
  }

  if (strlen(WIFI_SSID)) {
    DEBUG_PRINTF("[net] trying built-in network '%s'" "\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    if (wifiWaitConnected(WIFI_CONNECT_MS)) {
      netState = NET_UP;
      return;
    }
  }

  // Nothing worked.  Say so on the panels, because a head sitting dark with
  // no explanation looks broken rather than unconfigured.
  DEBUG_PRINTF("[net] no network; opening setup portal '%s'" "\n",
               WIFI_AP_NAME);
  showMessage("WIFI", "SETUP", "join the network", WIFI_AP_NAME);

  netState = NET_PORTAL;
  WiFiManager wm;
  wm.setHostname(WIFI_HOSTNAME);
  wm.setConfigPortalTimeout(WIFI_PORTAL_S);
  wm.setConfigPortalBlocking(true);
  bool ok = wm.startConfigPortal(WIFI_AP_NAME);
  netState = ok ? NET_UP : NET_DOWN;
  if (!ok)
    DEBUG_PRINTF("[net] portal timed out; carrying on offline" "\n");
}

// Everything that only makes sense once there is a link.
void netOnConnected(void) {
  if (WiFi.status() != WL_CONNECTED)
    return;
  // Link-local IPv6 is not brought up by default, and takes a moment to be
  // assigned, so the address can still read as :: right after boot.
  WiFi.enableIpV6();
  if (MDNS.begin(WIFI_HOSTNAME))
    DEBUG_PRINTF("[net] mdns up: %s.local" "\n", WIFI_HOSTNAME);
  else
    DEBUG_PRINTF("[net] mdns failed to start" "\n");
  DEBUG_PRINTF("[net] connected: %s  ipv4 %s" "\n",
               WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  netStartTime();
  webBegin();
}

// Address cards, one panel each, because IPv6 will not fit beside the rest:
// a link-local address is around 24 characters and a 128 px panel holds 21.
// Splitting across the two displays is the tidiest use of having two.
uint32_t netShowUntil = 0;
void netShow(void); // defined with the display code below


// Applies the timezone and kicks off SNTP.  Safe to call again after a TZ
// change: the daemon is simply reconfigured.
// Typing a POSIX string correctly is no fun, so the common zones get names.
// A raw POSIX string is still accepted for anywhere not listed.

const TzChoice tzChoices[] = {
    {"pacific", "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"mountain", "MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"arizona", "MST7"}, // no DST
    {"central", "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"eastern", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"alaska", "AKST9AKDT,M3.2.0/2,M11.1.0/2"},
    {"hawaii", "HST10"}, // no DST
    {"uk", "GMT0BST,M3.5.0/1,M10.5.0/2"},
    {"europe", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"utc", "UTC0"},
};
const uint8_t numTzChoices = sizeof(tzChoices) / sizeof(tzChoices[0]);

// Returns the POSIX string for a shortcut, or NULL if the name is unknown.
const char *tzLookup(const char *name) {
  for (uint8_t i = 0; i < numTzChoices; i++)
    if (!strcasecmp(name, tzChoices[i].name))
      return tzChoices[i].posix;
  return NULL;
}

// Whether a sync has ever landed.  The clock free-runs until it has, so the
// eyes work with no network at all.
bool timeSynced = false;
char tzString[TZ_MAX] = TZ_DEFAULT;

void netStartTime(void) {
  configTzTime(tzString, NTP_SERVER_1, NTP_SERVER_2);
}

// Non-blocking check, polled until the first sync lands.  SNTP replies take
// a second or two, and blocking on it would stall the eyes for no reason.
void netPollTime(void) {
  if (timeSynced || WiFi.status() != WL_CONNECTED)
    return;
  struct tm t;
  if (!getLocalTime(&t, 0)) // 0 = do not wait
    return;
  // The epoch starts at 1970; anything before ~2021 means SNTP has not
  // actually answered yet and we are seeing the power-on default.
  if (t.tm_year < (2021 - 1900))
    return;
  timeSynced = true;
  DEBUG_PRINTF("[net] time synced: %04d-%02d-%02d %02d:%02d:%02d %s" "\n",
               t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min,
               t.tm_sec, tzString);
}

void netReport(Print &out) {
  out.printf("host=%s.local state=%s" "\n", WIFI_HOSTNAME,
             WiFi.status() == WL_CONNECTED ? "up"
             : netState == NET_PORTAL     ? "portal"
                                          : "down");
  out.printf("  mac  %s" "\n", WiFi.macAddress().c_str());
  if (WiFi.status() == WL_CONNECTED) {
    out.printf("  ssid %s (%d dBm)" "\n", WiFi.SSID().c_str(),
               (int)WiFi.RSSI());
    out.printf("  ipv4 %s  gw %s" "\n", WiFi.localIP().toString().c_str(),
               WiFi.gatewayIP().toString().c_str());
    out.printf("  ipv6 %s" "\n", WiFi.localIPv6().toString().c_str());
  }
  if (timeSynced) {
    struct tm t;
    getLocalTime(&t, 0);
    out.printf("  time %04d-%02d-%02d %02d:%02d:%02d  tz %s" "\n",
               t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
               t.tm_min, t.tm_sec, tzString);
  } else {
    out.printf("  time not synced (tz %s)" "\n", tzString);
  }
}


void netDrawPanel(uint8_t e) {
  GFXcanvas1 c(PANEL_W, PANEL_H);
  c.fillScreen(0);
  c.setTextColor(1);
  splashCenter(c, WIFI_HOSTNAME, 2, 4);
  c.drawFastHLine(14, 26, PANEL_W - 28, 1);

  if (WiFi.status() != WL_CONNECTED) {
    splashCenter(c, "OFFLINE", 2, 52);
    splashCenter(c, "no network", 1, 80);
    pushCanvas(e, c);
    return;
  }

  int16_t y = 34;
  if (e == 0) {
    char line[24];
    splashCenter(c, "MAC", 1, y);
    y += 11;
    splashCenter(c, WiFi.macAddress().c_str(), 1, y);
    y += 18;
    splashCenter(c, "IPv4", 1, y);
    y += 11;
    splashCenter(c, WiFi.localIP().toString().c_str(), 1, y);
    y += 18;
    snprintf(line, sizeof(line), "%d dBm", (int)WiFi.RSSI());
    splashCenter(c, line, 1, y);
  } else {
    splashCenter(c, "IPv6", 1, y);
    y += 11;
    String v6 = WiFi.localIPv6().toString();
    // Wrapped rather than truncated: a partial address is worse than useless.
    for (uint16_t i = 0; i < v6.length(); i += NET_COLS) {
      splashCenter(c, v6.substring(i, i + NET_COLS).c_str(), 1, y);
      y += 10;
    }
    y += 10;
    splashCenter(c, WIFI_HOSTNAME ".local", 1, y);
  }
  pushCanvas(e, c);
}

// Paints both panels and leaves them up for a while.  Non-blocking: frame()
// simply skips the eye render until the deadline, so the console stays
// responsive and a second `net` refreshes rather than queueing.
void netShow(void) {
  for (uint8_t e = 0; e < displayCount(); e++)
    netDrawPanel(e);
  netShowUntil = millis() + NET_SHOW_MS;
}

#endif // NETWORK
