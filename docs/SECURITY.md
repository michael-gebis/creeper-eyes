# Who is allowed to drive the head

None of this is switched on. A prop on a home network, where the only things
that can reach it are things you already let onto your WiFi, is a perfectly
reasonable place to stop — and simplicity is a legitimate choice, so making it
costs nothing: with the switches off, none of this code is compiled in at all.

If you do want it, the pieces are independent and you can take one without the
others. The one worth doing regardless is first.

Credentials live in `include/secrets.h` beside the WiFi ones, and are never
committed. See [`include/secrets.h.example`](../include/secrets.h.example).

## The one worth doing anyway

**Over-the-air updates have no password unless you set one.** Without it,
anything on your network can flash whatever firmware it likes onto the board
— a larger hole than the web interface being open, and a cheaper one to
close. There is no switch: define it and it applies.

```c
#define OTA_PASSWORD "choose-something"    // in include/secrets.h
```

Uploading then needs it, from the environment rather than a committed file:

```sh
OTA_PASSWORD=choose-something pio run -e gray_ota -t upload
```

That sets the initial one. It can be changed later from the control page
without a rebuild — see [Changing the passwords](#changing-the-passwords).

## The web interface

`-DAUTH_HTTP=1` puts **digest** authentication on the control page, the REST
API and `/cmd`. Digest rather than basic because the password is never sent
— only a hash of it with a server nonce — which matters because this device
cannot practically serve HTTPS (see below). Browsers handle the challenge
themselves and ask once.

`-DAUTH_TOKEN=1` adds a bearer token as an alternative, for scripts that
would rather not do digest:

```sh
curl --digest -u frank:... http://frank.local/api/v1/state
curl -H "Authorization: Bearer ..." http://frank.local/api/v1/state
```

The token is sent in the clear on every request, so it is the weaker of the
two — make it long and random. Either may be used on its own or both together.

`-DAUTH_HOST_CHECK=1` refuses requests whose `Host` header does not name this
device. That is the defence against **DNS rebinding**, which authentication
alone does not stop: a page you visit can make your own browser call
`192.168.x.x`, and a browser holding cached credentials will attach them.

Turning any of them on with no password defined **fails the build** rather
than producing a device that looks protected and is not.

## Changing the passwords

The credentials start as whatever `include/secrets.h` was built with, and the
**Passwords** card on the control page replaces them without a rebuild. All
three: the page password, the update password, and the script token. A stored
value overrides the built-in one; a board that has never been told otherwise
behaves exactly as it always did.

Three things are worth knowing before using it.

**They cross the network in clear text.** Digest protects the password on
every *later* request — it is never sent, only hashed with a server nonce —
but it cannot protect the one request that carries a new password in its body,
and there is no HTTPS to fall back on. Set them once, from a machine on the
same network.

**The update password only takes effect after a reboot.** Not a choice:
`ArduinoOTA` refuses a new password once one is set, `end()` does not clear
it, and there is no way to replace it in a running firmware. The card says so
when one is pending.

**A board whose API needs no credential will refuse to change one.** Build
without `AUTH_HTTP` and `AUTH_TOKEN` and the endpoint returns 403. This
matters most in the configuration that looks safest: an `OTA_PASSWORD` with no
API authentication, where the update password is the only thing between the
network and arbitrary firmware. An open endpoint that sets passwords is a back
door however politely it is written.

Passwords are stored in plain text, because `WebServer::authenticate()` takes
a plaintext password and offers no variant accepting a digest HA1, and
hand-rolling digest verification is exactly the code that is wrong in ways
nobody notices. NVS is not encrypted, so anyone who can dump the flash can
read them. The update password is the exception — `ArduinoOTA` wants an MD5
hash, so only the hash is kept.

## Forgetting the password

Hold the **BOOT** button for ten seconds while the board is running. The
panels count down from five seconds in, so a press that is about to wipe the
board says so first; let go and nothing happens. At zero it erases every
stored setting, restores the built-in credentials and reboots.

The gesture needs `COMMANDS` (the default), since that is what polls the
button at all.

It has to be a long press *while running*, not a hold at power-on, because
`BOOT_BUTTON_PIN` is GPIO0 — the strapping pin — and holding that low through
a reset puts the ESP32 into its serial bootloader instead of running this
firmware at all.

This is the only way back into a sealed head whose password has been
forgotten, which is why it needs physical access and cannot be triggered over
the network.

## Why there is no HTTPS

Not for want of a certificate — a real one can be had for a private address
through DNS-01. Three other reasons:

- The Arduino core has **no TLS server**. `WiFiClientSecure` is client-side.
  Using a third-party one would mean rewriting every route.
- A handshake is **one to two seconds of ESP32 CPU**, and this web server is
  polled from inside the render loop. Every page load would stall the eyes.
- Self-signed means a browser warning forever, on every device.

Digest answers most of the same question at none of that cost. If you want
real TLS, terminate it on something else — a Pi or a NAS in front of the
board — and leave Frank speaking plain HTTP on a segment you trust.
