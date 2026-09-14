# Getting Frank onto your network

Everything here is optional. `NETWORK=0` compiles the whole lot out and the
eyes carry on exactly as they always did — the head does not need a network to
be a head.

With one, Frank joins your WiFi, serves a control page, keeps its clock from a
time server, and accepts firmware over the air. This page covers getting it
connected and keeping it there; [CONTROL.md](CONTROL.md) covers driving it
once it is, and [SECURITY.md](SECURITY.md) covers who is allowed to.

## Credentials

Copy the template and fill it in — it is gitignored, so nothing secret is ever
committed:

```sh
cp include/secrets.h.example include/secrets.h
```

```c
#define WIFI_SSID "YourNetwork"
#define WIFI_PASS "YourPassword"
```

It is optional, and skipping it is the easier route. A build without
`secrets.h` still compiles, and an unconfigured board opens a **setup
portal** — [which you set up from the eye](#setting-up-from-the-eye),
with a phone, without editing anything.

Three sources are tried in order of how deliberate they are: whatever the
portal last stored, then the build-time defaults, then the portal. Stored
credentials win because they were an explicit choice made on that device.

Nothing here is fatal — a board that cannot reach a network carries on being a
pair of eyes.

> **Why a header rather than `secrets.ini` and `build_flags`**, which would be
> the more idiomatic PlatformIO route: `build_flags` are processed by SCons,
> which uses `$` as its own substitution character. A password containing `$`
> is silently truncated there — no error, just a shorter string and a board
> that will not associate. Doubling the `$` does not help. The preprocessor
> reads a header directly, so only ordinary C string escaping applies.

## Setting up from the eye

A head with no network it recognises opens its own, called `frank-setup`,
and shows you how to get on it:

- **His left eye** — the one on your right, as you face him — shows a code.
  Point a phone camera at it and the phone joins the network, without you
  typing the name or the password, and the setup page opens by itself. Pick
  your WiFi, put in its password, done.
- **His right eye** shows the same thing as text: the network name, the
  password, and how long is left. That is the way in for anyone whose phone
  will not scan, and it is why the password avoids `O`/`0` and `I`/`1`/`L`.

The password is new every time the portal opens, and random — not derived
from the MAC, which is broadcast in every beacon frame and so would be a
password anyone in range could work out. Having one at all matters more
than it looks: the page you type *your* WiFi password into is served over
that network, and until there was a password on it, that network was open
to anyone nearby.

The portal gives up after `WIFI_PORTAL_S` seconds and the head carries on
offline — but the clock restarts for as long as somebody is connected to
it, so it will not close while you are still typing.

What a phone can genuinely read off a 128×128 panel sunk into an eye socket
was measured rather than assumed. [Codes on the eyes](QR.md) has the
numbers, including the version that did not work and why more error
correction made it worse.

`QR_CODES=0` leaves the text card alone, and takes the portal password with
it — a password nobody can read is worse than none.

## Changing networks later

The portal is not the only way in once the head is sealed. `wifi join`, over
serial or from the control page, stores a network and reboots into it:

```
> wifi join spare-network hunter2
ok storing 'spare-network'; rebooting
```

`wifi portal` reboots into the setup portal on demand, and `wifi forget`
clears the stored network so the build-time credentials apply again.

Each of these reboots rather than reconnecting in place. Reconnecting would
mean re-running mDNS, SNTP, the web server and OTA and getting every one of
them idempotent; rebooting reuses the path that already works, and the board
is back in about eight seconds.

The three sources of credentials are genuinely distinct: the build-time
defaults are applied with the radio's storage set to RAM, so connecting with
them does not quietly turn them into a stored network — otherwise `wifi
forget` would look like it had not worked the moment the board reconnected.

## Driving it

The control page, the serial console, the REST API and the `/cmd` escape
hatch all reach the same operations underneath — see
[docs/CONTROL.md](CONTROL.md).

## Address info on the panels

`net` reports over serial and paints both panels for twelve seconds — Frank's
right shows MAC, IPv4 and signal, and his left a code that opens the control
page, so nobody has to read an address off an eye and type it correctly into
a phone.

The code carries `http://192.168.1.50/` rather than `http://frank.local/`,
for the reason [OTA gives](#over-the-air-updates) for not relying on the
name: mDNS is missing on Windows without Bonjour and unreliable on older
Android. A dotted quad works anywhere on the subnet, and because the code is
drawn fresh each time it is always current.

Without `QR_CODES`, his left shows the mDNS name and which network he is on
instead. Anything longer than the 21 characters a panel holds is wrapped
rather than truncated, since half an address is worse than none. With `IPV6`
turned on his left shows the IPv6 address, code or no code — showing that
address is the whole reason to build with `IPV6` at all.

Twelve seconds is a long time to stare at a MAC address, so the cards can be
dismissed: `net off` over serial, the same button on the control page, or
`PUT /api/v1/netinfo {"on":false}`. `net quiet` reports without touching the
panels at all.

## IPv6

**Compiled out.** `IPV6` in [`src/config.h`](../src/config.h) is `0`, and with
it the address is not brought up, not reported, and not printed on the
address cards.

The reason is that it could not be used for anything. `WiFiServer` in the
ESP32 Arduino core opens an `AF_INET` socket and nothing else, so nothing
listens on the v6 address — it answers pings and refuses HTTP. And the
address `enableIpV6()` brings up is link-local, reachable only from the same
segment and only with a zone index in the URL
(`http://[fe80::…%2528]/`), so it would be a poor service address even if
something were listening. An address on screen that cannot be connected to is
just one more thing to be puzzled by.

Two things have to change before it earns its place: the core has to move to
3.x, where the server is dual-stack, and the board needs a global address,
which needs the router to advertise a prefix. Setting `IPV6` to `1` brings
the lot back at once when they do.

`GET /api/v1/net` reports `"ipv6Served": false` either way, so a client that
finds no `ipv6` field can tell why. Use the IPv4 address or `frank.local`.

## Booting with no network

A prop should be a prop whether or not the WiFi is up, so the boot order puts
the eyes first:

| | |
| :--- | :--- |
| **0.9 s** | Panels up, settings restored, RTC read if one is fitted |
| **0.9 s** | **Splash** — the panels name themselves for `SPLASH_SECONDS` |
| **6 s** | Stored network tried, then the build-time one (`WIFI_CONNECT_MS` each) |
| **36 s** | Setup portal, if neither worked — with a countdown on the panels |
| **96 s** | Gives up, and the eyes run |

Worst case is about a minute and a half, and the panels are showing something
throughout. The portal exits the moment a connection appears, so a network
that is simply slow costs seconds rather than the full timeout.

**A network that turns up later is picked up without a reboot.** Every
`WIFI_RETRY_MS` the board tries again, and when it succeeds mDNS, the web
server, OTA and NTP all come up as if they had at boot. This is not just
watching for a link: the setup portal tears the association down when it
times out, so nothing would be trying otherwise — measured on the bench, a
board left after a failed portal never reconnects on its own.

### The clock hides itself when nothing knows the time

With no network, no RTC, and nothing typed in, the clock face is switched off
rather than drawn from the free-running counter that starts at 10:10 — a
confident-looking clock showing the wrong time is worse than no clock:

```
> clock
clock on, hidden -- the time is unknown 10:10:10 rate=1x seconds=on
```

The setting is not changed, so the face comes back on its own the moment
anything supplies a time — a `clock set`, an RTC, or the network arriving.
The control page says the same thing on the clock card.

## Over-the-air updates

Use [`tools/ota.py`](../tools/ota.py):

```sh
python tools/ota.py --host frank.local
python tools/ota.py --host 192.168.1.50 --env gray_rtc_ota
```

It builds, uploads, and then asks the board whether the update took. Progress
shows on the panels too — the eyes stop during the transfer, which is expected
and not a hang.

Pick the `--env` matching the build the board is *running*: `gray_rtc_ota` for
a board built as `gray_rtc`. `gray_ota` extends `gray`, so using it on an RTC
build would quietly flash away the RTC support and the authentication — an
update that succeeds and leaves a different device behind, with no serial port
to undo it.

**Why not `pio run -t upload`.** That calls `espota.py`, which on a weak link
reports failure for updates that have already succeeded — three times in four,
measured here. The bug is in the acknowledgements, not the transfer. espota
performs exactly one `recv()` for every 1024-byte chunk it sends, while the
board acknowledges once per read of up to *1460* bytes. While the board keeps
up the counts coincidentally match; as soon as the link stalls and data backs
up in the board's buffer, one read swallows two chunks and answers once. From
there espota is an acknowledgement behind for the rest of the file, and ends
blocking on a `recv` that never comes — printing "Error Uploading" having
delivered every byte.

`tools/ota.py` implements the protocol itself and does not count. It streams
the image and drains acknowledgements as they arrive, letting TCP supply the
backpressure espota was trying to impose by hand. It also binds the *UDP*
socket to the chosen interface, not just the listening socket: the board
connects back to whatever address the invitation came from, which is the other
half of why OTA is a coin toss on a machine with VMware, WSL and VirtualBox
each contributing an interface.

Then it asks the board anyway, because an uploader saying "done" and a device
running the new firmware are different claims. It polls `/api/v1/info` until
the commit matches the one just built and uptime has reset, and retries only
what genuinely failed.

One thing still bites on Windows: **`frank.local` will not resolve** unless
Bonjour is installed, since Windows has no mDNS resolver of its own. The
device advertises correctly — pass the address instead.

macOS and Linux resolve it without help.

A transfer is about 1350 round trips, so it is exposed to a weak link in a way
a single request is not, and this link drops about 6% of its packets — see
[docs/HTTP_LATENCY.md](HTTP_LATENCY.md). `tools/ota.py` retries what
genuinely failed. If it keeps failing, check `GET /api/v1/net` for the signal
before suspecting the firmware: on this board the radio has been the cause of
every timing problem measured so far.
