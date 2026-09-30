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

Uploading then needs it. [`tools/ota.py`](../tools/ota.py) reads it from
`secrets.h`, or takes `--password`:

```sh
uv run tools/ota.py --host frank.local --env gray_rtc_ota
```

That sets the initial one. It can be changed later from the control page
without a rebuild — see [Changing the passwords](#changing-the-passwords).

## The web interface

`-DAUTH_HTTP=1` puts **digest** authentication on the control page, the REST
API and `/cmd`. Digest rather than basic because the password is never sent
— only a hash of it with a server nonce — which matters because this device
cannot practically serve HTTPS (see below). Browsers handle the challenge
themselves and ask once.

A nonce lasts `AUTH_NONCE_S`, five minutes, whoever is asking. A request
carrying an older one is challenged with `stale=true`, which tells the browser
to retry with the new nonce rather than ask the person again; only a wrong
password presented with the current nonce reaches a prompt. The lifetime also
bounds how long a captured request could be replayed. (The web server library
made a fresh nonce for every challenge and never said `stale`, so one script
being challenged sent every open control page to a password prompt.)

An answer is also held to the request it was computed for. Digest names the
address it covers inside the header, and the web server library checked the
answer against that address without comparing it with the request actually
made. So one header seen on the network opened every route taking the same
method, `/cmd` included. The page's own once-a-second poll would have
supplied one. The board now requires the path and query named in the header
to be the request's own. What that cannot stop is the same request being sent
again within the nonce's five minutes. Nor, since digest covers no body, can
it stop a captured `PUT` or `POST` being resent with a different body.
Closing that takes TLS; see below.

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
alone does not stop. A page you visit points its own name at `192.168.x.x`,
so your browser calls the board and attaches the credentials it has cached.

The same page can simply call `frank.local` or the board's address instead.
Those requests do name this device, so the Host check cannot tell them from
the page's own. So whenever a credential is required, the board refuses any
request the browser says **another site sent**: `Sec-Fetch-Site` on current
browsers, `Origin` or `Referer` on older ones. Following a link to the
control page from somewhere else still opens it; anything else another site
sends gets a 403. The page also tells browsers never to show it inside
another site's frame, where its one-click controls could be clicked on your
behalf. A very old browser that sends none of those headers can still be
made to follow a plain link to `/cmd`. Current Chrome, Firefox and Safari all
send them.

Uploading an [eye file](EYE_FILES.md#security) is the one request whose body
arrives before its handler runs, so it makes the same check itself before a
byte reaches flash.

Turning on `AUTH_HTTP` without `AUTH_USER`/`AUTH_PASS`, or `AUTH_TOKEN`
without `AUTH_TOKEN_VALUE`, **fails the build** rather than producing a device
that looks protected and is not. So does either of them without
`OTA_PASSWORD`, for the same reason: a password on the web interface and none
on updates would leave the firmware open to anything on the network. An empty
string counts as missing. `AUTH_HOST_CHECK` needs no secret and builds on its
own. Any of the three with `NETWORK=0` also fails, there being nothing to
authenticate.

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
stored setting, restores the built-in credentials and reboots. It leaves the
WiFi network alone, which the radio stores for itself, and the eye slot.

The gesture needs `COMMANDS` (the default), since that is what polls the
button at all.

It has to be a long press *while running*, not a hold at power-on, because
`BOOT_BUTTON_PIN` is GPIO0 — the strapping pin — and holding that low through
a reset puts the ESP32 into its serial bootloader instead of running this
firmware at all.

### `forget` does this too, and it is reachable over the network

The credentials live in the same NVS namespace as every other setting, so the
ordinary **forget** — the button on the Settings card, `forget` at the
console, or `POST /api/v1/settings {"op":"forget"}` — clears them along with
the eye design and the timezone. At the next boot the board is back on the
credentials its firmware was built with.

That is deliberate: a factory reset that left a password behind would not be
one, and on a sealed head the reset is the only way back in. But it has a
consequence worth stating plainly, because nothing else on this page implies
it: **anyone who can already authenticate can put the credentials back to the
built-in ones**, remotely, in one request. They cannot read the current
password — no endpoint reveals it — but they can replace the whole set.

If that matters for where your head lives, the defence is that the built-in
credentials are themselves a secret: they come from `include/secrets.h`, which
is gitignored, so a reset returns the board to *your* password rather than to
a published default or to no password at all.

The BOOT gesture remains the way back in when nobody can authenticate, which
is the case it exists for, and it is the only one of the two that works from
outside a locked-out board.

## The setup portal has a password now

It is easy to miss that the portal is a security surface at all. It is not the
control page, it holds no settings, and it is up for sixty seconds. But it is
the page you type *your home WiFi password* into, and it is served over a
network the head itself creates — so for as long as it was open, that password
went in over a network anyone in range could join.

`frank-setup` now has a password, random per session, and the head shows it:
as text on his right eye, and inside the code on his left that a phone camera
turns into a one-tap join. That is the only reason a password is reasonable
here — an unattended prop cannot be asked for a secret nobody has been given.

It is random rather than derived from the board, which was the first idea and
a bad one: the obvious thing to derive it from is the MAC address, and the MAC
is broadcast in every beacon frame. A MAC-derived password is one that anyone
close enough to see the network can compute.

`PORTAL_PASSWORD=0` restores the open portal. `QR_CODES=0` sets it to `0` on
its own, deliberately: a password the panels cannot show is one nobody can
get past.

Nor is the portal a way to install firmware. WiFiManager, the library that
serves it, comes with an update page of its own. That page takes a firmware
file from anyone who has joined the setup network, with no password, which
would make the portal a way round `OTA_PASSWORD`. The firmware answers those
addresses itself before the library can, so no upload reaches it, and it
removes the page from the portal's menu.

The portal opens when the head has no network it recognises. The BOOT reset
above does not clear the stored network, so a head keeps its WiFi through one;
`wifi forget` or `wifi portal` is the way to the portal on purpose.

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
