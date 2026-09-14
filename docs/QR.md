# QR codes on the panels

Setting a head up means joining it to WiFi, and joining it to WiFi means
finding `frank-setup` in a list of networks and then finding the right page.
A QR code on the eye itself could replace both steps — a phone camera reads
it, joins the network, and the captive portal opens the page by itself.

The question was never whether an ESP32 can draw a QR code. It was whether a
phone can *read* one off a 128×128 panel sunk into an eye socket. This page is
the answer, measured rather than assumed.

## The short version

**It works, at version 3 or below.** The join code scans reliably from a range
of distances, with the panels mounted in their real positions in the head
rather than held conveniently on a bench. The feature is viable.

| | payload | version | modules | drawn | panel margin | result |
| :-- | :-- | :-- | :-- | :-- | :-- | :-- |
| WiFi join, ECC M | 41 bytes | 3 | 29×29 | 111 px | 8.5 px | **reliable** |
| WiFi join, ECC Q | 41 bytes | 4 | 33×33 | 123 px | 2.5 px | reads, but the phone struggles to latch |
| Address, ECC M | 23 bytes | 2 | 25×25 | 99 px | 14.5 px | **reliable** |

Every one of them renders at **three pixels per module** — 128 divided by
anything in this range is three. So module size is not the variable. What
changes is how many modules there are and how much panel is left over.

## Why version 4 lost

More error correction sounds like the safer choice and is not. ECC Q buys
25% recovery against ECC M's 15%, but it pushes the code from version 3 to
version 4: thirteen percent more modules to resolve, across a code that fills
123 of the panel's 128 pixels with two and a half pixels to spare.

Error correction only helps *after* a decoder has locked onto the code. It
does nothing for the step that was failing, which is finding it at all. The
denser code was harder to see and no easier to read.

## The flicker, which was not what it looked like

Pointed at a camera, the panels beat visibly. The obvious suspect — the sketch
redrawing too fast — was wrong: each code is drawn once and then left alone
for fifteen seconds, so there was nothing of ours to slow down.

It was the panel scanning itself. `SSD1327::begin()` sent `0xB3 = 0x00`, the
slowest oscillator setting the controller has, and a slow panel scan beats
against a rolling shutter. Stepping `0xB3` through its range with one fixed
code on screen found `0xF0` — the fastest available — steady.

`begin()` sends `0xF0` now, so this is fixed for the eyes as well. The panels
had presumably always refreshed that slowly; nobody had noticed because nobody
had pointed a camera at them.

It took two goes. The commit that announced this fix added `setFrontClock()`
and `setPhaseLength()` — the accessors the sweep needs — and never touched the
value `begin()` sends. The sweep worked, `0xF0` was confirmed on hardware, this
page said it was fixed, and the eyes went on scanning at `0x00` for another
ten commits. What caught it was reading the diff before a merge and noticing
that the comment on `setFrontClock()` still described `begin()` as sending
`0x00` — which it did.

Worth the warning: a fix verified through a diagnostic path is verified for the
diagnostic path. The sweep proved the *value*, which was the hard part and the
part that felt like the work; it could not prove the value had been written
down anywhere that runs.

## Where the codes are

Both of them are on Frank's left eye, because both are things you point a
phone at and his right eye is busy saying the same thing in words for
anyone who cannot.

| when | left eye | right eye |
| :-- | :-- | :-- |
| the setup portal is open | code that joins `frank-setup` | network name, password, seconds left |
| `net`, or the button on the control page | code that opens `http://<address>/` | MAC, IPv4, signal |

Neither is a separate mode to find. The setup code appears whenever a head
has no network it recognises, which is the moment it is needed; the address
code appears on the address cards, which already existed.

Each says on the console what it drew:

```
> net
ok showing address cards for 12s -- `net off` to dismiss
[net] address code: http://192.168.123.166/
```

```
[net] no network; opening setup portal 'frank-setup'
*wm:StartAP with SSID:  frank-setup
[net] join code: WIFI:T:WPA;S:frank-setup;P:Y7DTFVY3;;
```

Those lines are worth their space. A panel that drew nothing and a panel that
drew the wrong thing look identical from across a room, and telling them apart
otherwise means pointing a phone at an eye and guessing. For the portal it
matters more: the board is not on a network while the portal is up — that is
what the portal is for — so serial is the only channel there is.

The portal line carries the password, which is deliberate. It is random per
session and already on a panel for anyone in the room to read; whoever is
watching the console has a cable in the board, which is nearer than that.

## Why the address code carries an address

`http://192.168.123.166/`, not `http://frank.local/`.

The name needs mDNS, which Windows does not have without Bonjour and which
Android only resolves reliably from 12 onward — the same trap the README
already documents for over-the-air updates. A QR that opens `frank.local`
would simply fail for a lot of people.

A dotted quad works anywhere on the subnet, and because the code is drawn live
it is always current, which a printed label could never be. It costs nothing:
23 bytes is still version 2, and fifteen characters is as long as an IPv4
address gets, so that is the worst case rather than a flattering one.

## Does the whole thing work

Yes, verified end to end on hardware:

1. The portal opens and the right eye shows the join code.
2. A phone camera reads it off the eye and joins `frank-setup` — nobody
   types the network name or the password.
3. The captive portal opens the setup page by itself.
4. The network is chosen, the board connects, and comes back on it.

Which found a crash that had nothing to do with QR codes.

`wm.stopConfigPortal()` had been called unconditionally after the portal
loop since long before any of this. When the portal *succeeds*,
WiFiManager's own `process()` has already called `shutdownConfigPortal()`,
which resets the server and clears the active flag — so the second call
dereferenced a null server and panicked, `LoadProhibited`, inside
`shutdownConfigPortal()` itself.

It crashed on every successful setup and on no failed one. Every test that
ended in a sixty-second timeout passed, which is every test anyone had run:
until the join code made the portal quick enough to drive all the way
through, nobody had ever completed a setup. Making a feature usable is a way
of reaching the code nobody reaches.

Guarded with `getConfigPortalActive()`.

## What the test does not tell us

- **Anything about other phones.** One camera, one decoder, one pair of
  panels. Version 3 had margin at every distance tried, which is the reason
  to expect it travels, but that is an expectation and not a measurement.
- **Anything about lighting.** Everything here was read off a self-lit panel
  indoors. A head in direct sun, or behind the tinted lens a finished prop
  might have, is untested.
- **How well it survives the socket.** The panels were in their real
  positions, but a deeper socket narrows the angle a camera can read from,
  and nothing here says how much.

## A trap worth remembering

The first run of this test reported version 1, 21×21, for every payload —
including the 41-byte one, which version 1 cannot hold at any error correction
level.

`qrcode.c` carries `@TODO: Return error if data is too big` directly above its
init function, and means it: any length returns success. The fitting loop
tried version 1 first, trusted the return value, and stopped. The panels
displayed crisp, well-formed grids of nothing.

Had anyone scanned them, the failure would have read as *"phones cannot manage
this density"* — the exact opposite of the truth, and the conclusion that
would have killed the feature. The capacity table from ISO/IEC 18004 is now
checked before the library is asked to encode anything.

Each code is also dumped to serial as text. Scan that off a monitor: if it
reads there but not on the panel, the encoding is right and the problem is
optics.

## Running it

```sh
pio run -e qrtest -t upload -t monitor        # SSD1327 grey
pio run -e qrtest_rgb -t upload -t monitor    # SSD1351 colour
```

**USB only.** There is no network stack in this build, so flashing it over the
air leaves the board unreachable until somebody plugs a cable in.

The right eye shows the code; the left eye says which one, so a photograph of
the pair documents itself.
