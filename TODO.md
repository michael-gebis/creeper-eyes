# TODO

Things worth doing, not yet started: features that do not exist and bugs
that have not been fixed. Nothing here has been acted on. Where a section
carries measurements, they are the ones that shaped the entry rather than
results from working code. An entry may record a decision taken about it
— including a decision not to build it, and why — without any of it
having been written.

## Online firmware updates

**Not being built, as of 2026-09-22.** A judgement call rather than a blocked
one: the head works, `tools/ota.py` updates it from the author's machine in
under two minutes, and everything below is a large amount of new surface — a
second way into the device, credentials that have to move before it can be
used at all, and a failure mode whose worst case is a brick in a sealed head
— in exchange for convenience the one person currently updating a head does
not need. The reasoning below is kept because it is the expensive part and it
does not spoil: picking this up again is a decision, not a fresh
investigation.

A head that can update itself, rather than one that waits for somebody with a
checkout of this repository and a working PlatformIO install.

Today's over-the-air update still starts on a computer: `pio run -t upload` or
[`tools/ota.py`](tools/ota.py) pushes a binary the operator already has. See
[Networking](docs/NETWORK.md) for what exists now.

### Three goals, which are not the same size

1. **No PlatformIO and no checkout needed to update a head.**
2. **The head fetches new firmware itself, from the internet.**
3. **The head notices new firmware exists and says so.**

The actual pain is (1). (2) and (3) are considerably larger, and only (2)
needs GitHub at all. Doing them in order means each step is useful on its own
and none of them blocks on the one after it.

### What the hardware already gives us

`partitions.csv` has two app slots and an `otadata` partition (and, since
[eye files](docs/EYE_FILES.md), an eye slot beside them):

| partition | offset | size |
| :-- | :-- | :-- |
| `otadata` | `0xE000` | 8 KB |
| `app0` | `0x10000` | 1.81 MB |
| `app1` | `0x1E0000` | 1.81 MB |

`gray_rtc` builds to about 1.25 MB now that it carries one eye design, so
there is roughly 600 KB of headroom in a slot — enough for TLS if it comes to
that, but not a lot.

Rollback is compiled into the core the project already builds against:
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` and `CONFIG_APP_ROLLBACK_ENABLE=y`.
A freshly flashed slot boots in `ESP_OTA_IMG_PENDING_VERIFY`, and
`esp32-hal-misc.c` confirms it automatically at boot — *unless* the weak
`verifyRollbackLater()` is overridden to return true, at which point
confirming becomes ours to do with
`esp_ota_mark_app_valid_cancel_rollback()`. So the mechanism is there and
opting into it is one function.

### Two prerequisites that are specific to this project

Neither is about networking, and both are the real work.

**The build matrix.** There are sixteen environments. A head needs the binary
matching its panel type, its RTC and its auth flags, and a wrong pick is a
brick. The device does know what it is — `GET /api/v1/info` already reports
which features are compiled in — so it can ask for a named variant. But it
means publishing roughly ten binaries per release, and it means the naming
scheme becomes an interface that cannot casually change.

*Decided, 2026-09-22: publish two.* `gray_rtc` — greyscale panels, an RTC,
`AUTH_HTTP` / `AUTH_TOKEN` / `AUTH_HOST_CHECK`, which is what the author's
head runs — and the same build with colour panels, which does not exist as an
environment yet: `esp32dev_rtc` is colour with an RTC but carries none of the
auth flags, so the twin is either that environment with the three flags added
or a new one beside it. One `-DUSE_SSD1327` apart, and the device already
knows which of the two it is, so variant matching stops being a problem worth
designing for. Everything else — no RTC, no authentication, the `_open`
builds, the diagnostics — is built from source by whoever wants it and is not
published at all.

The cost of that choice is that the published pair cannot be the binaries the
author builds today, for the reason that follows.

**Credentials are compiled in.** `gray_rtc` requires `AUTH_USER`, `AUTH_PASS`
and `AUTH_TOKEN_VALUE` in `include/secrets.h` and refuses to build without
them. A published binary cannot contain anybody's secrets, so a generic
release binary arrives with no built-in credentials at all.

`credBegin()` prefers NVS over the built-ins and NVS survives an OTA, so a
board whose credentials are stored is fine. The author's board is not: it
reports `stored: false` for all four and runs on the built-ins. So online
updates need, first:

- a way to move built-in credentials into NVS deliberately, and
- an `AUTH_HTTP=1` build that tolerates having no built-ins, refusing access
  rather than allowing it when NVS is also empty.

So the two published variants are `gray_rtc` and its colour twin *built
without credentials*, which is a slightly different firmware from the one the
author runs — and on the author's own head the order matters. The credentials
have to reach NVS **before** the first credential-less binary is flashed onto
it, or that head comes up with no way in: no built-ins, an empty store, and
`PUT /api/v1/credentials` sitting behind the very authentication that now has
nothing to check against. Once they are stored the published binary and the
private one behave identically, and the author is then running the thing
everybody else downloads, which is the best test it can get.

`WIFI_SSID` / `WIFI_PASS` need the same move and are the safe case: a head
with no stored network opens the setup portal and shows a join code, so it can
be recovered without being opened.

### The plan, in the order the steps are worth doing

**Step 1 — upload a `.bin` from the control page.** Browser to head, over the
LAN. No internet, no TLS, no certificate authority, no GitHub, no manifest,
and no variant matching: the operator picks the file, so "which binary" is
answered by the person who knows. This alone retires goal (1), which is the
only goal anybody is actually blocked by today.

**Step 2 — fetch a URL typed into the control page.** The same flow with the
download moved onto the device. Still no version checking and still no
GitHub; the operator is still choosing.

**Step 3 — a signed manifest.** A small JSON file naming the current version,
a URL per variant, and a hash. The head compares against its own
`FIRMWARE_VERSION`, and offers rather than takes.

### On GitHub Releases specifically

It is a reasonable instinct: free, versioned, already where the code lives,
release notes for nothing. Three things bite.

- **HTTPS is mandatory**, and an asset download redirects to a different host,
  so the device must follow a 302 *and* complete a second handshake against a
  different chain. Root CAs get pinned into firmware, and when GitHub rotates
  them every head in the field loses its update path permanently, silently,
  years later. This is the way this feature usually breaks.
- **Rate limits.** Sixty requests an hour per IP unauthenticated. Fine for a
  manual check, bad for polling, and worse for several heads behind one NAT.
- **TLS costs.** mbedTLS wants tens of KB of heap during the handshake,
  alongside the panel canvases and the web server, plus a couple of hundred KB
  of flash against that 600 KB of headroom.

**Signing the binary makes most of that go away.** With a public key baked
into the firmware and a signature checked before the slot is marked bootable,
the transport stops needing to be trusted: fetch over plain HTTP, drop
mbedTLS, and the certificate-expiry time bomb disappears with it. GitHub then
becomes an ordinary file host and could be swapped for any other. Which
algorithm and which implementation is an open question, not a decision.

### Non-negotiable once a binary can arrive without a cable

- **Verify before marking the slot bootable** — hash at minimum, signature
  preferably — rather than after.
- **Confirm on evidence, not on booting.** Override `verifyRollbackLater()`
  and call `esp_ota_mark_app_valid_cancel_rollback()` only once the new
  firmware has joined WiFi and served a page. A firmware that boots but cannot
  be reached is indistinguishable from a brick in a sealed head, and not
  opening a sealed head again is the premise of the project.
- **Never update unasked.** A prop that reboots itself in the middle of
  Halloween is a fault, whatever the changelog said. The same reasoning
  already sets `SLEEP_ENABLED` to 0 by default.

### Open questions

- ~~Which variants are worth publishing~~ — two, above. What they are *called*
  is still open, and it is the half that becomes an interface: a name
  published once has to keep meaning the same thing.
- Whether step 3 is wanted at all, or whether step 2 is where this should
  stop.
- Whether an update should be refused while the clock says it is near or
  inside the sleep window, on the grounds that nobody is watching to see it
  fail.

## A light sensor for automatic brightness

The [dimmer](docs/BRIGHTNESS.md) exists; what it lacks is a reason to move by
itself. A light-dependent resistor and a fixed resistor, as a divider on one of
the DevKit's input-only pins, would let the eyes turn themselves down in a dark
room and up in a bright one.

- **Pins.** `D34`, `D35`, `VP` (GPIO36) and `VN` (GPIO39) are all unused on
  the carrier board and are all ADC1, which keeps working with WiFi up (ADC2
  does not). On a rev A or rev B board it is three jumpers; a later revision
  could carry a footprint for it.
- **Where it plugs in.** Another factor in `dimmerPoll()`'s target, beside
  sleep's: the setting stays what the person chose, and the sensor scales it.
- **Open questions.** Where the sensor sits so it sees the room and not the
  eyes' own glow; how much smoothing, so a passing shadow does not flicker the
  eyes; and whether it should only ever dim, never brighten past the setting.

## An unexplained watchdog reset on frank-dev

On 2026-09-27 frank-dev, the colour board, reported "the board restarted after
a watchdog". Its uptime put the reset about half an hour after it was updated
over the air to the `overlap-send` branch (8f75d4f) on 2026-09-25, late in
the evening with nothing testing it; it then ran 36 hours without another.
It has no C1, runs at 160 MHz, and has sleep mode off.

- **Why it matters.** `OVERLAP_SEND` keeps core 0 busy sending frames, and the
  task watchdog watches core 0's idle task. The sender steps aside for one
  tick a second so that the idle task runs; if something defeats that, this
  is what it would look like. But it could as well be the interrupt watchdog,
  or another, and at the time the report did not say which.
- **What now records it.** Since 482ea55 the report names the watchdog, and
  for the task watchdog the task that kept core 0 busy, sampled at every tick
  (a forced test named the culprit correctly). frank-dev runs that firmware.
- **Not reproduced.** 20 minutes of `tools/load_test.py` against it afterwards
  passed without a restart; so did three 45-minute runs before the reset.
- **Next time it happens,** the warning says which task. `panels` would point
  at the sender's rest, which could step aside more often at a small cost in
  frame rate.

## The panel bus speed as a setting

The CPU speed is a setting now; the speed of the SPI bus each panel is driven
at is still a build option, `SSD1351_SPI_HZ` or `SSD1327_SPI_HZ`. It was
discussed beside the CPU speed and not built. Lowering it is the cure for a
panel that flickers or speckles, so as a setting it would let a sealed head be
turned down without a rebuild. It would belong on the same card as the CPU
speed.

## Three pictures

The only photograph in the repository is a screenshot, and it is out of
date. Each of these has a place already waiting for it; none is written
yet.

- **`docs/images/webui.png`, retaken.** The control page as it was on
  2026-09-10, before the **flip** buttons, the eye-file controls, the
  brightness slider and the warnings box joined it. It is the first thing the README shows and the first thing in
  [Driving it](docs/CONTROL.md), so it is the picture most people will
  compare against what they see. Same file name, same two places; nothing
  else references it.

- **Frank in action.** A photo of the head with the eyes running, for the
  top of the README. Today the README opens with the control page, which
  shows what the project *does to* the eyes rather than what they look
  like; the head itself is what anyone landing on the page wants to see
  first, and the screenshot can move down to the "Takes direction" line it
  illustrates. Dim room, eyes lit, ideally mid-glance so the gaze reads as
  a gaze and not a stare.

- **The ESP32 on a breadboard, wired to both panels.** For
  [Wiring](docs/WIRING.md), somewhere between "Connections" and "Order of
  assembly". The table there is complete and the prose is careful, but
  fourteen wires is the point at which a photo says in one look what the
  table says in fourteen rows — which side of the module the panels sit,
  where the rails are used, how the shared lines are daisy-chained. Taken
  from above, in enough light to follow a wire from pin to pin.
