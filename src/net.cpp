// WiFi, the setup portal, mDNS and network time.  See net.h.

#include "net.h"

#if NETWORK

#include "display.h"
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_wifi.h>
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
// Its own namespace rather than the console's: this is the network module's
// business, and keeping it separate means `forget` on the settings side cannot
// wipe the flag by accident.
#define NET_PREFS "frank-net"
#define NET_KEY_PORTAL "portal"

static uint8_t pendingOp = 0; // 0 none, 1 join, 2 forget, 3 portal
static char pendingSsid[33];
static char pendingPass[64];

// What NVS held when this boot started.  Captured once, because it is only
// true once: esp_wifi_get_config() returns the driver's *running* config,
// which is loaded from NVS at init but overwritten by the first begin().
static char bootSsid[33];

// WiFi.SSID() reports the access point currently associated, not what is in
// NVS: it calls esp_wifi_sta_get_ap_info(), which is empty until a connection
// exists.  Using it to ask "do we have stored credentials?" therefore always
// answered no, and the stored-network branch below had never once run -- a
// board configured through the portal went back to the portal on every boot.
static void readStoredSsid(void) {
  wifi_config_t conf;
  bootSsid[0] = '\0';
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK)
    return;
  strncpy(bootSsid, (const char *)conf.sta.ssid, sizeof(bootSsid) - 1);
  bootSsid[sizeof(bootSsid) - 1] = '\0';
}

bool netStoredSsid(char *out, size_t n) {
  if (!out || n == 0)
    return false;
  strncpy(out, bootSsid, n - 1);
  out[n - 1] = '\0';
  return out[0] != '\0';
}

bool netRequestJoin(const char *ssid, const char *pass) {
  if (!ssid || !*ssid || strlen(ssid) > 32)
    return false;
  if (pass && strlen(pass) > 63)
    return false;
  strncpy(pendingSsid, ssid, sizeof(pendingSsid) - 1);
  pendingSsid[sizeof(pendingSsid) - 1] = 0;
  strncpy(pendingPass, pass ? pass : "", sizeof(pendingPass) - 1);
  pendingPass[sizeof(pendingPass) - 1] = 0;
  pendingOp = 1;
  return true;
}

void netRequestForget(void) { pendingOp = 2; }
void netRequestPortal(void) { pendingOp = 3; }
bool netRebootPending(void) { return pendingOp != 0; }

// Whether the last boot was asked to go straight to the portal.  Reading it
// clears it, so a portal request is honoured exactly once.
static bool takePortalRequest(void) {
  Preferences p;
  if (!p.begin(NET_PREFS, false))
    return false;
  bool want = p.getBool(NET_KEY_PORTAL, false);
  if (want)
    p.remove(NET_KEY_PORTAL);
  p.end();
  return want;
}

void netPollPending(void) {
  uint8_t op = pendingOp;
  if (!op)
    return;
  pendingOp = 0;

  switch (op) {
  case 1:
    DEBUG_PRINTF("[net] storing network '%s' and rebooting" "\n", pendingSsid);
    showMessage("WIFI", "JOIN", pendingSsid, "rebooting");
    // persistent(true) is set in setupNetwork, so begin() writes the
    // credentials to NVS.  The connection attempt itself is incidental --
    // the reboot is what applies them, through the usual path.
    WiFi.begin(pendingSsid, pendingPass);
    break;
  case 2:
    DEBUG_PRINTF("[net] forgetting the stored network and rebooting" "\n");
    showMessage("WIFI", "FORGET", NULL, "rebooting");
    WiFi.disconnect(true, true); // radio off, erase the stored AP
    break;
  case 3:
    DEBUG_PRINTF("[net] portal requested; rebooting into it" "\n");
    showMessage("WIFI", "SETUP", NULL, "rebooting");
    {
      Preferences p;
      if (p.begin(NET_PREFS, false)) {
        p.putBool(NET_KEY_PORTAL, true);
        p.end();
      }
    }
    break;
  }

  delay(600); // long enough for the panels to be read, and the socket to drain
  ESP.restart();
}

void setupNetwork(void) {
  // Hostname before mode() and begin(), or the DHCP request goes out with
  // the default name and the router records that instead.  Learned the hard
  // way on the wandering-hour-clock.
  WiFi.persistent(true);
  WiFi.setHostname(WIFI_HOSTNAME);
  WiFi.mode(WIFI_STA);

  // Modem sleep off.  The default parks the radio between DTIM beacons,
  // which costs hundreds of milliseconds on every round trip -- measured at
  // a 1.7 s median for one small GET, with a tenth of them past eight
  // seconds.  The web server is polled from the render loop and serves one
  // client at a time, so that latency does not queue politely: it stacks up
  // requests until whatever is talking to the board gives up.  Roughly 30 mA
  // more, which is nothing for a prop that lives on a USB lead.
  WiFi.setSleep(false);

  bool forcePortal = takePortalRequest();
  if (forcePortal)
    DEBUG_PRINTF("[net] portal was requested; skipping stored networks" "\n");

  readStoredSsid(); // before any begin() overwrites the running config

  char savedSsid[33];
  if (!forcePortal && netStoredSsid(savedSsid, sizeof(savedSsid))) {
    DEBUG_PRINTF("[net] trying stored network '%s'" "\n", savedSsid);
    WiFi.begin();
    if (wifiWaitConnected(WIFI_CONNECT_MS)) {
      netState = NET_UP;
      return;
    }
  }

  if (!forcePortal && strlen(WIFI_SSID)) {
    DEBUG_PRINTF("[net] trying built-in network '%s'" "\n", WIFI_SSID);
    // Deliberately not written to NVS.  The driver persists whatever begin()
    // is given, which would quietly turn the build-time fallback into a
    // stored network -- and then `wifi forget` would look like it had not
    // worked, because something would still be stored the moment the board
    // reconnected.  Only a portal setup or an explicit join get to persist.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    bool ok = wifiWaitConnected(WIFI_CONNECT_MS);
    esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (ok) {
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
#if IPV6
  // Link-local IPv6 is not brought up by default, and takes a moment to be
  // assigned, so the address can still read as :: right after boot.
  WiFi.enableIpV6();
#endif
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

// Enough of the world to cover wherever the head ends up.  These are POSIX
// TZ strings, not the IANA database -- the database is megabytes and needs a
// filesystem, while a POSIX string is thirty bytes and is what the C library
// wants anyway.  The trade is that a country changing its DST rules needs a
// firmware update, which for a Halloween prop is the right side of the deal.
// Anything not listed can still be set: `tz` takes a raw POSIX string.
//
// Offsets are inverted relative to how people say them: UTC+2 is written -2.
// Zones without DST are a single field.
const TzChoice tzChoices[] = {
    // North America
    {"los_angeles", "North America", "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"denver", "North America", "MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"phoenix", "North America", "MST7"},
    {"chicago", "North America", "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"new_york", "North America", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"halifax", "North America", "AST4ADT,M3.2.0/2,M11.1.0/2"},
    {"st_johns", "North America", "NST3:30NDT,M3.2.0/2,M11.1.0/2"},
    {"anchorage", "North America", "AKST9AKDT,M3.2.0/2,M11.1.0/2"},
    {"honolulu", "North America", "HST10"},
    {"mexico_city", "North America", "CST6"}, // DST abolished in 2022
    {"panama", "North America", "EST5"},

    // South America
    {"bogota", "South America", "<-05>5"},
    {"lima", "South America", "<-05>5"},
    {"caracas", "South America", "<-04>4"},
    {"santiago", "South America", "<-04>4<-03>,M9.1.6/24,M4.1.6/24"},
    {"sao_paulo", "South America", "<-03>3"}, // DST abolished in 2019
    {"buenos_aires", "South America", "<-03>3"},

    // Europe
    {"reykjavik", "Europe", "GMT0"},
    {"london", "Europe", "GMT0BST,M3.5.0/1,M10.5.0/2"},
    {"dublin", "Europe", "GMT0IST,M3.5.0/1,M10.5.0/2"},
    {"lisbon", "Europe", "WET0WEST,M3.5.0/1,M10.5.0/2"},
    {"madrid", "Europe", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"paris", "Europe", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"berlin", "Europe", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"rome", "Europe", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"warsaw", "Europe", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"athens", "Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"helsinki", "Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"kyiv", "Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"moscow", "Europe", "MSK-3"},

    // Africa and the Middle East
    {"casablanca", "Africa / Middle East", "<+01>-1"},
    {"lagos", "Africa / Middle East", "WAT-1"},
    {"cairo", "Africa / Middle East", "EET-2EEST,M4.5.5/0,M10.5.4/24"},
    {"johannesburg", "Africa / Middle East", "SAST-2"},
    {"jerusalem", "Africa / Middle East", "IST-2IDT,M3.4.4/26,M10.5.0"},
    {"nairobi", "Africa / Middle East", "EAT-3"},
    {"istanbul", "Africa / Middle East", "<+03>-3"},
    {"riyadh", "Africa / Middle East", "<+03>-3"},
    {"tehran", "Africa / Middle East", "<+0330>-3:30"},
    {"dubai", "Africa / Middle East", "<+04>-4"},

    // Asia
    {"karachi", "Asia", "PKT-5"},
    {"kolkata", "Asia", "IST-5:30"},
    {"kathmandu", "Asia", "<+0545>-5:45"},
    {"dhaka", "Asia", "<+06>-6"},
    {"bangkok", "Asia", "<+07>-7"},
    {"jakarta", "Asia", "WIB-7"},
    {"singapore", "Asia", "<+08>-8"},
    {"hong_kong", "Asia", "HKT-8"},
    {"shanghai", "Asia", "CST-8"},
    {"taipei", "Asia", "CST-8"},
    {"manila", "Asia", "PST-8"},
    {"seoul", "Asia", "KST-9"},
    {"tokyo", "Asia", "JST-9"},

    // Oceania
    {"perth", "Oceania", "AWST-8"},
    {"adelaide", "Oceania", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
    {"brisbane", "Oceania", "AEST-10"},
    {"sydney", "Oceania", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"melbourne", "Oceania", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"auckland", "Oceania", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
    {"fiji", "Oceania", "<+12>-12"},

    // Universal
    {"utc", "Universal", "UTC0"},
};

// The names this project shipped with before the list went worldwide.  Kept
// working because they are documented and people have them in scripts, but
// left out of tzChoices so the picker offers one name per place.
static const struct {
  const char *alias;
  const char *of;
} tzAliases[] = {
    {"pacific", "los_angeles"}, {"mountain", "denver"},
    {"arizona", "phoenix"},     {"central", "chicago"},
    {"eastern", "new_york"},    {"alaska", "anchorage"},
    {"hawaii", "honolulu"},     {"uk", "london"},
    {"europe", "paris"},
};
const uint8_t numTzChoices = sizeof(tzChoices) / sizeof(tzChoices[0]);

// Returns the POSIX string for a shortcut, or NULL if the name is unknown.
const char *tzLookup(const char *name) {
  for (uint8_t i = 0; i < numTzChoices; i++)
    if (!strcasecmp(name, tzChoices[i].name))
      return tzChoices[i].posix;
  for (uint8_t i = 0; i < sizeof(tzAliases) / sizeof(tzAliases[0]); i++)
    if (!strcasecmp(name, tzAliases[i].alias))
      return tzLookup(tzAliases[i].of);
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
#if IPV6
    out.printf("  ipv6 %s" "\n", WiFi.localIPv6().toString().c_str());
#endif
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
#if IPV6
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
#else
    splashCenter(c, "NAME", 1, y);
    y += 11;
    splashCenter(c, WIFI_HOSTNAME ".local", 1, y);
    y += 18;
    splashCenter(c, "NETWORK", 1, y);
    y += 11;
    // Wrapped for the same reason as the address was: an SSID can be 32
    // characters and a panel holds 21.
    String ssid = WiFi.SSID();
    for (uint16_t i = 0; i < ssid.length(); i += NET_COLS) {
      splashCenter(c, ssid.substring(i, i + NET_COLS).c_str(), 1, y);
      y += 10;
    }
#endif
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

// Take the panels back before the deadline.  frame() resumes rendering on its
// next pass, so there is nothing to redraw here.
void netHide(void) { netShowUntil = 0; }

bool netShowing(void) { return netShowUntil != 0; }

#endif // NETWORK
