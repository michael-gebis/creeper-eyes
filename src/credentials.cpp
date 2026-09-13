// See credentials.h.

#include "credentials.h"

#if NETWORK

#include <MD5Builder.h>
#include <Preferences.h>

// Kept in the same namespace as every other setting, so that "forget" and the
// factory reset clear these too.  That is deliberate: a factory reset that
// left a password behind would not be one, and on a sealed head the reset is
// the only way back in after a forgotten password.
#define CRED_NAMESPACE "creeper"

// NVS keys are limited to fifteen characters.
static const char *const KEYS[CRED_COUNT] = {
    "authUser",
    "authPass",
    "authToken",
    "otaHash",
};

// What the firmware was built with.  Each may be absent: a build without
// AUTH_HTTP has no AUTH_USER, and one without OTA_PASSWORD has no hash to
// fall back to.
static const char *builtIn(CredKind k) {
  switch (k) {
#ifdef AUTH_USER
  case CRED_USER:
    return AUTH_USER;
#endif
#ifdef AUTH_PASS
  case CRED_PASS:
    return AUTH_PASS;
#endif
#ifdef AUTH_TOKEN_VALUE
  case CRED_TOKEN:
    return AUTH_TOKEN_VALUE;
#endif
  default:
    break;
  }
  return "";
}

static String live[CRED_COUNT];  // the value in force
static bool stored[CRED_COUNT];  // ...and whether it came from NVS
static bool rebootPending = false;

// MD5 of a password, hex, which is the form ArduinoOTA wants.
static String md5Of(const char *s) {
  MD5Builder md5;
  md5.begin();
  md5.add(s);
  md5.calculate();
  return md5.toString();
}

void credBegin(void) {
  Preferences prefs;
  prefs.begin(CRED_NAMESPACE, true); // read-only
  for (int i = 0; i < CRED_COUNT; i++) {
    String s = prefs.getString(KEYS[i], "");
    stored[i] = s.length() > 0;
    live[i] = stored[i] ? s : String(builtIn((CredKind)i));
  }
  prefs.end();

#if OTA_AUTH
  // The built-in OTA password is a password, not a hash, so it needs hashing
  // to match what a stored one already is.
  if (!stored[CRED_OTA])
    live[CRED_OTA] = md5Of(OTA_PASSWORD);
#endif
}

const char *credGet(CredKind k) {
  if (k < 0 || k >= CRED_COUNT)
    return "";
  return live[k].c_str();
}

bool credIsStored(CredKind k) {
  return (k >= 0 && k < CRED_COUNT) && stored[k];
}

bool credRebootPending(void) { return rebootPending; }

bool credSet(CredKind k, const char *value, String &err) {
  if (k < 0 || k >= CRED_COUNT) {
    err = "no such credential";
    return false;
  }
  String v(value ? value : "");

  if (!v.length()) {
    // Refusing this is the whole point of having a check: an empty password
    // stored over a real one turns a protected board into an open one that
    // still claims to be protected.
    err = "must not be empty";
    return false;
  }
  if (v.length() > 63) {
    err = "must be 63 characters or fewer";
    return false;
  }
  for (size_t i = 0; i < v.length(); i++) {
    char c = v[i];
    if (c < 0x20 || c == 0x7f) {
      err = "must not contain control characters";
      return false;
    }
  }
  // A colon in the username breaks digest authentication outright: the hash
  // is over "user:realm:password", so a colon makes the fields ambiguous and
  // the browser and the board would disagree about where one ends.
  if (k == CRED_USER && v.indexOf(':') >= 0) {
    err = "a username may not contain a colon";
    return false;
  }

  String toStore = (k == CRED_OTA) ? md5Of(v.c_str()) : v;

  Preferences prefs;
  if (!prefs.begin(CRED_NAMESPACE, false)) {
    err = "could not open storage";
    return false;
  }
  bool ok = prefs.putString(KEYS[k], toStore) > 0;
  prefs.end();
  if (!ok) {
    err = "could not write storage";
    return false;
  }

  live[k] = toStore;
  stored[k] = true;
  if (k == CRED_OTA)
    rebootPending = true; // see credRebootPending()
  return true;
}

void credForget(void) {
  // The values themselves are cleared by whoever wipes the namespace; this
  // just puts the running copy back to what the firmware was built with, so
  // the board does not keep honouring a credential it has just forgotten.
  for (int i = 0; i < CRED_COUNT; i++) {
    stored[i] = false;
    live[i] = String(builtIn((CredKind)i));
  }
#if OTA_AUTH
  live[CRED_OTA] = md5Of(OTA_PASSWORD);
#endif
  rebootPending = true;
}

#endif // NETWORK
