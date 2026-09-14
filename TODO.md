# TODO

Things worth doing, not yet started. Nothing on this page exists in the
firmware; it is a plan, and the measurements in it are the ones that shaped
the plan rather than results from a working feature.

## Online firmware updates

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

`min_spiffs.csv` has two app slots and an `otadata` partition:

| partition | offset | size |
| :-- | :-- | :-- |
| `otadata` | `0xE000` | 8 KB |
| `app0` | `0x10000` | 1.9 MB |
| `app1` | `0x1F0000` | 1.9 MB |

`gray_rtc` builds to about 1.4 MB, so there is roughly 500 KB of headroom in a
slot — enough for TLS if it comes to that, but not a lot.

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

`WIFI_SSID` and `WIFI_PASS` have the same shape, with a better safety net: a
head that comes up on a generic binary with no network now opens the setup
portal and shows a join code, so it can be recovered without being opened.
That lowers the risk of this whole feature considerably and is worth stating
as one of the reasons it is now worth attempting.

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
  of flash against that 500 KB of headroom.

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

- Which variants are worth publishing, and what are they called.
- Whether step 3 is wanted at all, or whether step 2 is where this should
  stop.
- Whether an update should be refused while the clock says it is near or
  inside the sleep window, on the grounds that nobody is watching to see it
  fail.
