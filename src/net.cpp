// WiFi, the setup portal, mDNS and network time.  See net.h.

#include "net.h"

#if NETWORK

#include "display.h"
#include "rtc.h"
#include "timekeeping.h"
#include <esp_sntp.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <WiFi.h>
#include <WiFiManager.h>

uint8_t netState = NET_DOWN;

// Whether to consult a time server at all.  Declared here, above the
// first user, because netOnConnected() reads it long before the SNTP
// plumbing further down is defined.
static bool ntpWanted = true;

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
  if (ntpWanted)
    netStartTime();
  else
    DEBUG_PRINTF("[net] ntp is off; not starting the time client" "\n");
  webBegin();
}

// Address cards, one panel each.  A 128 px panel holds 21 characters of the
// default font, which is not enough for everything worth reading off a head
// at arm's length, so having two displays is the tidiest way out: Frank's
// right takes the numbers, his left takes the names.  Anything that still
// does not fit is wrapped rather than truncated -- half an address is worse
// than none.
uint32_t netShowUntil = 0;
void netShow(void); // defined with the display code below

// Applies the timezone and kicks off SNTP.  Safe to call again after a TZ
// change: the daemon is simply reconfigured.
// Typing a POSIX string correctly is no fun, so the common zones get names.
// A raw POSIX string is still accepted for anywhere not listed.

// Returns the POSIX string for a shortcut, or NULL if the name is unknown.

// Whether a sync has ever landed.  The clock free-runs until it has, so the
// eyes work with no network at all.

// Set from the SNTP task the moment a reply is applied, and cleared by
// netPollTime.  The work that follows a sync -- logging it, writing it
// through to the RTC -- is done from the render loop rather than here,
// because this runs in another task and an I2C transaction does not belong
// in it.
static volatile bool ntpArrived = false;
static volatile uint32_t ntpLastMs = 0; // 0 = no server has ever answered

static void onSntpSync(struct timeval *tv) {
  timeAccept(tv->tv_sec, TIME_NTP);
  ntpLastMs = millis();
  ntpArrived = true;
}

bool netNtpEnabled(void) { return ntpWanted; }

void netNtpSetEnabled(bool on) {
  ntpWanted = on;
  if (!on) {
    if (sntp_enabled())
      sntp_stop();
    // The clock keeps whatever the server last said, but stops being
    // defended by it -- otherwise `clock set` would have no effect on a board
    // whose time server has been switched off.
    timeRelinquish(TIME_NTP);
    DEBUG_PRINTF("[net] ntp off; the time already set is kept" "\n");
    return;
  }
  // Only worth starting once there is something to ask over; otherwise
  // netOnConnected() will start it when the link arrives.
  if (WiFi.status() == WL_CONNECTED)
    netStartTime();
}

void netNtpStatus(NtpStatus &o) {
  o.enabled = ntpWanted;
  o.running = sntp_enabled();
  o.linkUp = WiFi.status() == WL_CONNECTED;
  o.synced = ntpLastMs != 0;
  o.lastSyncSec = o.synced ? (millis() - ntpLastMs) / 1000UL : NTP_NEVER;
  o.intervalSec = sntp_get_sync_interval() / 1000UL;
  o.server = NTP_SERVER_1;
}

bool netNtpSyncNow(void) {
  // Restarting the client makes it query straight away instead of waiting out
  // the remaining interval.  Only meaningful if it was started in the first
  // place, which it is not before a link exists.
  if (!sntp_enabled())
    return false;
  return sntp_restart();
}

void netStartTime(void) {
  // A callback rather than watching for the year to look sane: with an RTC
  // fitted the clock is already right at boot, so "is it past 2021 yet" can
  // no longer tell a real sync from a restored one.  This fires exactly when
  // SNTP applies a reply, and never otherwise.
  sntp_set_time_sync_notification_cb(onSntpSync);
  configTzTime(tzString, NTP_SERVER_1, NTP_SERVER_2);
  timeApplyTz(); // configTzTime sets TZ too; this keeps the one owner honest
}

// Non-blocking check, polled until the first sync lands.  SNTP replies take
// a second or two, and blocking on it would stall the eyes for no reason.
void netPollTime(void) {
  if (!ntpArrived)
    return;
  ntpArrived = false;

  struct tm t;
  if (timeLocal(t))
    DEBUG_PRINTF("[net] time synced: %04d-%02d-%02d %02d:%02d:%02d %s" "\n",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour,
                 t.tm_min, t.tm_sec, tzString);

#if RTC
  // The whole point of the battery: a board that has seen the network once
  // keeps the right time through a power cut, and through the network going
  // away for good.
  if (rtcPresent() && rtcWriteNow())
    DEBUG_PRINTF("[rtc] written from ntp" "\n");
#endif
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
  timeReport(out);
}

// Paints one address card.  Frank's right takes the numbers -- MAC, IPv4,
// signal -- and his left the names, because a 128 px panel holds 21
// characters of the default font and none of this fits on one.
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
