# Eye files

One eye design beyond the built-in ones, loaded from a file through the control
page and kept on the board until it is replaced or removed. The firmware ships
with `default` only. Any of the other designs in the [gallery](EYES.md#gallery)
can go in the slot without rebuilding anything.

## Using it

1. Get an eye file (`dragon.bin`, say), either built with
   [`tools/make_eye.py`](../tools/make_eye.py) as below or downloaded from a
   release.
2. On the control page, in the **Eye** card, choose the file under
   **load an eye file** and press **load**.
3. The panels read `EYE LOADING` for five to eight seconds, then the new design comes
   up and joins the eye list after the built-in ones.

Loading another file replaces the design already in the slot. **remove**
empties the slot. If the eyes were showing the removed design, they go back to
`default`. `save` remembers the design by name, as it does for any other, so a
head that was showing `dragon` when saved comes back up showing `dragon`.

The console can see the slot but cannot fill it, since a serial line is no way
to deliver 158 KB:

```
> eye
  0  default
  1  dragon      <- current   (loaded from a file)
> eye unload
ok slot empty; eye=0 default
```

### Building eye files

```sh
python tools/make_eye.py dragon        # one design -> dist/eyes/dragon.bin
python tools/make_eye.py --all         # every header in include/ -> dist/eyes/
```

It reads the same headers the firmware compiles in (`include/eyes/*.h`,
`include/defaultEye.h`, `include/newtEye.h`) and writes the tables out as raw
little-endian bytes behind a header. You don't need Pillow or a TeensyEyes
checkout for this. Those are only needed for
[regenerating the headers](EYES.md#adding-or-regenerating-designs).

## The one-time USB flash

The design lives in its own flash partition, `eyes`. A board's partition table
is written only by a USB flash. An over-the-air update replaces the firmware
but keeps the old table, so a head updated over the air runs the new firmware
without a slot:

- the control page says **this board has no eye slot** instead of offering an
  upload,
- `GET /api/v1/eyes/slot` reports `"available": false`,
- and everything else works as before.

Flash it once over USB (`pio run -e gray_rtc -t upload`) and the slot appears.
Settings, credentials and WiFi survive, because `nvs` has not moved. After
that, over-the-air updates work as they always have.

### Where the room came from

[`partitions.csv`](../partitions.csv) replaces the stock `min_spiffs.csv`. The
two app slots each give up 64 KB, and the unused SPIFFS partition goes:

| partition | offset | size | was |
| :-- | :-- | :-- | :-- |
| `nvs` | `0x9000` | 20 KB | unchanged |
| `otadata` | `0xE000` | 8 KB | unchanged |
| `app0` | `0x10000` | 1856 KB | 1920 KB |
| `app1` | `0x1E0000` | 1856 KB | 1920 KB |
| `eyes` | `0x3B0000` | 256 KB | *(SPIFFS, 128 KB, unused)* |
| `coredump` | `0x3F0000` | 64 KB | unchanged |

`coredump` stays because the Arduino core writes crash dumps to it. An eye file
is 158,400 bytes, so the slot has room for the format to grow.

Dropping `newt` from the default build frees about 158 KB of app space, which
more than covers the 64 KB each slot gave up.

## The file format

Little-endian throughout, like the ESP32 itself.

| offset | size | field |
| :-- | :-- | :-- |
| 0 | 8 | magic: the ASCII bytes `CREEPEYE` |
| 8 | 2 | format version: `1` |
| 10 | 2 | header size: `64` |
| 12 | 4 | payload size: `158336` |
| 16 | 16 | name, NUL-padded |
| 32 | 32 | SHA-256 of the payload |
| 64 | 158,336 | payload |

The payload is the five tables the renderer reads, one after another, each in
the same row-major layout as its C array:

| table | type | dimensions | bytes |
| :-- | :-- | :-- | --: |
| sclera | `uint16_t` RGB565 | 200 × 200 | 80,000 |
| iris | `uint16_t` RGB565 | 64 × 256 | 32,768 |
| upper eyelid | `uint8_t` | 128 × 128 | 16,384 |
| lower eyelid | `uint8_t` | 128 × 128 | 16,384 |
| polar | `uint16_t` | 80 × 80 | 12,800 |

Version 1 means exactly these dimensions. They're the ones the renderer is
compiled for, and the firmware checks at compile time that the two agree. A
file that differs in anything is rejected. The firmware never tries to adapt
one.

**The name** must be 1–15 characters of `a-z` and `0-9`, starting with a letter,
and must not be the name of a built-in design. Uniqueness is what lets `save`
store a design by name. The character set is what lets the name go straight
into JSON, the console and the page without escaping.

One file serves every build. Colour and greyscale panels render from the same
RGB565 tables, and the greyscale conversion happens as each frame is packed
for the panel.

## How it works

**No copy in RAM.** The board has no PSRAM, and 158 KB is about half the RAM it
has. The partition is mapped into the address space with `esp_partition_mmap()`,
and the renderer's five artwork pointers point straight into it. That's how the
compiled-in designs are read too: `const` arrays live in flash and are reached
through the same cache. `drawEye()` does not know which kind it is drawing.

**Streamed to flash.** `PUT /api/v1/eyes/slot` carries the file as the raw
request body. The web server hands it over in pieces of about 1.4 KB, and each
piece is written and hashed as it arrives, so the whole file is never in RAM.

**The header is written last.** The upload's first 64 bytes are held in RAM and
checked before anything is erased: magic, version, sizes and name, plus the
request's `Content-Length`. A file that is plainly wrong is refused with the
slot untouched. Once the header passes, the slot is erased, the payload is
written behind the header's space, and the hash is compared. Only if the hash
matches is the header itself written. A cut connection, a power cut, or a hash
mismatch at any point before that leaves a slot with no valid header, which the
board treats as empty. There is no half-loaded design that could be drawn.

**Checked again at boot.** Startup maps the slot, checks the header, and hashes
the payload before listing the design. This takes a few milliseconds, with the
ESP32's SHA hardware.

**Moving off before erasing.** If the eyes are showing the loaded design when a
new upload is accepted, or when `eye unload` runs, they switch to `default`
*before* the partition is erased or unmapped. Otherwise the renderer would be
holding pointers into memory that is about to change. That switch is immediate
rather than queued for the next frame, as other design changes are. It can be
immediate because web requests and console commands are both handled inside
the render loop, between frames, so nothing is mid-draw.

**The eyes stop while it loads.** Requests are served from the render loop, so
the upload is time without animation: 5–8 seconds over WiFi, measured on a
DevKit. Most of that is receiving 158 KB rather than writing it, and a file the
board refuses takes about as long, because the web server reads the whole body
before its handler can answer. The panels say `EYE LOADING` so a frozen eye
doesn't look like a crash.

### Security

An upload is a write to flash, so it gets the same credential check as every
other change, and one step more. The web server delivers a raw body *before*
the route's handler runs, and the handler is where the usual check lives. So
the upload path runs the same check itself, silently, before accepting a
single byte. An unauthenticated upload is read and thrown away, then refused
with the usual `401` or `403` by the handler. Nothing is erased. This also
covers a browser sending the body once without credentials and then again
with them.

Nothing in the file is executed. Only the 64-byte header is parsed, and every
field in it is checked against a fixed value or a fixed range. The payload is
pixels and lookup tables, read by a renderer that indexes each table only with
values bounded by its dimensions. The hash protects against corruption, not
against a malicious file. There is nothing to sign, because the worst a
well-formed file can do is look ugly.

## API

| method | path | |
| :-- | :-- | :-- |
| `GET` | `/api/v1/eyes/slot` | `{"available":true,"loaded":true,"name":"dragon","index":1,"capacity":262144}` |
| `PUT` | `/api/v1/eyes/slot` | the file as the body, `Content-Type: application/octet-stream`. Replies as `GET`, having selected the new design |
| `DELETE` | `/api/v1/eyes/slot` | empties the slot. Replies as `GET` |

Errors are JSON `{"error": "..."}` like everywhere else. A file that is not a
valid eye file gets `400`. A name that clashes with a built-in design, or a
board with no slot, gets `409`. A flash failure gets `500`.

```sh
curl -T dist/eyes/dragon.bin -H 'Content-Type: application/octet-stream' \
     http://frank.local/api/v1/eyes/slot
```

`GET /api/v1/eyes` lists the loaded design after the built-in ones, with
`"loaded": true`.
