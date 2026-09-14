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

## What the test does not tell us

- **Anything about other phones.** One camera, one decoder.
- **Whether the captive portal opens reliably** once joined. That is
  WiFiManager's DNS hijack, which already exists and is not exercised here.

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
