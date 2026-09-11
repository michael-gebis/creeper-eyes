# Why a 35-byte GET takes 50 ms

Notes for a change not yet made. The short version: every request costs about
a render frame, because that is how often the web server gets looked at, and
no amount of tuning inside that model removes it.

Measured on an ESP32 DevKit V1 with two SSD1327 panels, `gray_rtc` with
authentication on, rendering at 31–32 fps. Medians over 20 samples, taken with
`tools/test_api.py` and a curl timing breakdown.

## Where the time goes

| | bytes | tcp accept | handler | transfer | total | p90 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| `OPTIONS` preflight | 38 | 14 | 47 | 0 | 73 | 89 |
| `PUT /eye` | 38 | 10 | 31 | 2 | 44 | 66 |
| `GET /eyes` | 99 | 9 | 28 | 2 | 44 | 61 |
| `GET /state` | 943 | 13 | 31 | 2 | 52 | 81 |
| `GET /tz` | 2558 | 12 | 43 | 7 | 64 | 86 |
| `GET /` (gzipped) | 9364 | 11 | 39 | 78 | 117 | 203 |

The important row is the first one. `OPTIONS` parses no body, touches no
device state and returns 38 bytes, and it is not meaningfully faster than
anything else. **The work is not the cost.**

`GET /state` builds the largest JSON document the API produces — forty-odd
fields across eight objects, including an I²C read when an RTC is fitted — and
it is 8 ms slower than a 99-byte reply. Serialisation is free at this scale.

## What the cost actually is

`webPoll()` is called from `frame()`, once per rendered frame. At 31 fps that
is every 32 ms. A request that arrives just after a poll waits most of a frame
before anything looks at the socket.

That is the whole story. Accept, read, parse, dispatch and respond all happen
inside a single `handleClient()` call — it falls through from `HC_NONE`
straight into `HC_WAIT_READ` in the same invocation — so there is exactly one
poll in the path, and its cost is the polling interval.

## What was tried and did not work

**Draining the state machine.** The hypothesis was that `handleClient()`
advances one step per call, so a request needed three polls — accept, then
read, then dispatch — and calling it in a bounded loop would collapse three
frames into one.

It gained nothing. Measured with and without, the floor stayed at ~50 ms.

Two things were wrong with it:

- The premise. `handleClient()` does not need a second call; see above.
- The first attempt made things actively worse. An idle `handleClient()` ends
  in `delay(1)` to yield, so looping six times cost six milliseconds of *every*
  frame whether or not anyone was connected. The frame rate fell from 32 to 28
  immediately.

The loop was removed. The lesson worth keeping: measure the shape of the
problem before optimising it, and measure again afterwards even when the
change "obviously" helps.

## What did work

- **Gzipping the page** (`tools/gen_page.py`). 25,360 bytes became 9,069 — the
  send went from roughly seventeen TCP segments to six, and `GET /` from 554 ms
  to about 100. It also saved 14 KB of flash. This was by far the largest win,
  because it was the only change that reduced the number of round trips rather
  than the time per round trip.
- **Trimming `/tz`.** It was sending the POSIX string for all 61 zones, which
  no client reads. 3,943 bytes became 2,558, and the p90 went from **3617 ms**
  to 86. That tail was retransmission timeouts on a reply that spanned several
  segments; making it fit in fewer removed them.
- **Not polling a hidden tab.** A page left open in a background tab was
  spending a kilobyte and a slice of the render loop every second, forever.

The pattern: the wins came from sending less, not from working faster.

## The change not yet made: move the server off the render loop

The ESP32 has two cores. The Arduino `loop()` — and therefore the renderer and
the web server — runs on core 1. Core 0 runs the WiFi and lwIP stacks and is
otherwise largely idle.

Running the HTTP server in its own FreeRTOS task on core 0 would decouple
request latency from the frame rate entirely. The floor would become network
round-trip time, a couple of milliseconds on a LAN, instead of a frame.

```
  now                              proposed
  ┌──────── core 1 ────────┐       ┌──── core 1 ────┐  ┌──── core 0 ────┐
  │ loop()                 │       │ loop()         │  │ httpTask()     │
  │   frame()              │       │   frame()      │  │   handleClient │
  │     drawEye()          │       │     drawEye()  │  │     ↕ mutex    │
  │     webPoll()  ◄── 32ms│       │                │  │   state.h ops  │
  └────────────────────────┘       └────────────────┘  └────────────────┘
```

### What makes it harder than it looks

Every operation the API performs currently runs between frames, on the same
core as the renderer, which is why none of it needs a lock. That is the
property being given up.

Shared state that would need protecting:

- **The eye design pointers.** `setEyeDesign()` swaps five pointers that
  `drawEye()` dereferences as it scans. Changing them mid-render tears a frame.
  There is already a `swapPending` flag for the panel swap that exists for
  exactly this reason — the same discipline would have to cover eye selection,
  and the deferral would have to be driven from the render side.
- **The gaze and dilation targets.** Written by a handler, read every frame.
  Individually they are word-sized and benign; as a pair they can be read
  half-updated, which is a visible glitch rather than a crash.
- **The clock's colours and geometry.** Read per hand, per frame.
- **`Preferences`/NVS.** `save` performs a flash write. Doing that from
  another core while the renderer runs is fine, but two concurrent writers are
  not.
- **The panels themselves.** `netShow`, `showMessage` and the splash all push
  canvases over the same SPI bus the renderer uses. These must stay on the
  render side; a handler would have to request them, as `netinfo` and `splash`
  already do.

The honest assessment: the operations layer in `src/state.h` is the right seam
for this — it is already the single place device state is changed — but it was
written assuming a single thread, and making it thread-safe is a real piece of
work, not a decoration. A mutex around each operation plus a deferred-apply
flag for anything the renderer reads mid-frame is the shape of it.

### The alternative

`ESPAsyncWebServer` is event-driven and runs in the TCP task, which achieves
the same decoupling without a task of our own. It would mean rewriting all 17
routes, and it brings its own reputation for lifetime and memory bugs. It
would also make the same thread-safety demands of `state.h`, so it does not
avoid the hard part — only the scheduling part.

### Is it worth it

Probably not yet. 50 ms is imperceptible for a button press, and the control
page polls once a second. The case for it would be a client that wants to
drive the eyes smoothly from outside — the aim pad is already coalescing
around this limit, and a 50 ms floor caps that at 20 updates a second.

If it is done, the order should be:

1. Make `state.h` operations individually safe to call from another task.
2. Add the deferred-apply discipline for anything the renderer reads.
3. Only then move the server, so a failure at step 3 is a scheduling bug and
   not a data race.

## Open questions

- The measured wait is ~35 ms against a 32 ms frame. If `webPoll()` were
  evenly spaced the average should be ~16 ms. Where the poll sits relative to
  the SPI push has not been established, and moving the call might be a
  cheap improvement on its own.
- The p90 outliers that remain (`GET /rtc` occasionally at 663 ms) have not
  been explained. They look like retransmissions but nothing has confirmed it.
- `Connection: close` on every response means a TCP handshake per request,
  worth roughly 10 ms of the total. Keep-alive is not well supported by this
  `WebServer`, but the cost has not been measured against the risk.
