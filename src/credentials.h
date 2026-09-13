// The credentials, and where they are kept.
//
// Until now these were compile-time constants out of include/secrets.h, which
// meant changing a password was a rebuild and a reflash -- on a sealed head,
// over the air, using the very password you were trying to change.  This
// module gives them somewhere to live at runtime instead.
//
// The rule is the one config.h already uses for everything else: the
// compiled-in value is the default, and a stored value overrides it.  A board
// that has never been told otherwise behaves exactly as it did before, and
// nothing here changes which credentials are *required* -- that is still
// AUTH_HTTP, AUTH_TOKEN and OTA_PASSWORD at compile time.
//
// Two things are worth knowing before using any of it.
//
// The passwords are stored in plain text, because WebServer::authenticate()
// takes a plaintext password and offers no variant that accepts a digest HA1.
// NVS is not encrypted, so anyone who can dump the flash can read them.  That
// is a physical-access attack on a prop, and the alternative was
// reimplementing digest verification by hand -- which is exactly the kind of
// code that is wrong in ways nobody notices.  The OTA password is the one
// exception: ArduinoOTA takes an MD5 hash, so only the hash is kept.
//
// And setting a password sends it across the network in clear text, because
// there is no HTTPS (auth.h explains why there cannot be).  Digest protects
// the password on every *later* request; it cannot protect the one that
// carries the new password in a request body.  Set them from a machine on the
// same LAN, once, and prefer changing them rarely to changing them often.

#ifndef CREDENTIALS_H
#define CREDENTIALS_H

#include "config.h"

#if NETWORK

#include <Arduino.h>

// Which credential a call is talking about.
enum CredKind {
  CRED_USER,  // management username
  CRED_PASS,  // management password
  CRED_TOKEN, // bearer token, for scripts
  CRED_OTA,   // over-the-air update password (kept as an MD5 hash)
  CRED_COUNT
};

// Load the stored values, falling back to the compiled-in ones.  Call before
// authBegin() and otaBegin(), both of which read the results.
void credBegin(void);

// The value in force.  CRED_OTA returns the MD5 hash, never a password.
const char *credGet(CredKind k);

// Whether this credential has been changed from the value built into the
// firmware.  For the UI, which shows what is set without ever showing what it
// is set to.
bool credIsStored(CredKind k);

// Replace one credential and persist it.  Pass the plain password for
// CRED_OTA; the hash is computed here.  Returns false, with why in `err`, if
// the value is unusable -- empty, too long, or containing a colon where the
// digest algorithm would choke on one.
bool credSet(CredKind k, const char *value, String &err);

// True once something has changed that only a restart can apply: ArduinoOTA
// refuses to accept a new password after begin(), and offers no way to clear
// the one it has.
bool credRebootPending(void);

// Forget every stored credential, returning to the compiled-in defaults.
// Called by the factory reset; also what a cleared NVS leaves behind.
void credForget(void);

#endif // NETWORK
#endif // CREDENTIALS_H
