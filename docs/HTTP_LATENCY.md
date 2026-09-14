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
- **Polling a hidden tab fifteen times less often.** A page left open in a
  background tab was spending a kilobyte and a slice of the render loop every
  second, forever; it drops to once every fifteen.

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
the same decoupling without a task of our own. It would mean rewriting every
route, and it brings its own reputation for lifetime and memory bugs. It
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

## The prototype: what happened when it was built

It is on the `http-task` branch, behind `HTTP_TASK`, **defaulting to off**.

### The safety work came first, and it holds

Following the order above: `state.h` operations take a recursive mutex, and
anything the renderer reads mid-frame is queued and applied between frames by
`statePollPending()`. The eye design was the case that mattered — five
pointers `drawEye()` dereferences per pixel — and it is queued now, alongside
the panel swap that always was. `netShow()` became a request too, because it
painted, and the SPI bus belongs to the render loop.

That part works. Six clients hammering the board for forty seconds, changing
the eye, the gaze, the clock colours and the pupil at once, over eleven
hundred requests across several runs:

- no reboots
- no read-back mismatches — every value read back matched what had just been
  set, which is what a torn write would break
- heap flat to within a kilobyte
- the render loop kept going throughout

The same stress against the unmodified build behaves the same way, including
the occasional timeout under six-way concurrency: that is the single-client
server saturating, not the task.

### Where to put the task was not obvious

80 identical `GET /api/v1/state`, one configuration per row:

| | median | p90 | p99 | max | over 500 ms |
| :--- | ---: | ---: | ---: | ---: | ---: |
| on the render loop | 64 | 78 | 129 | 134 | 0 of 80 |
| core 0, priority 1 | — | — | — | — | the 9 KB page took **2.5 s** |
| core 1, priority 2 | 59 | 120 | 2690 | **5675** | 4 of 80 |
| core 0, priority 10 | 52 | 73 | 109 | 715 | 1 of 80 |

Core 0 at priority 1 is the trap: core 0 is where the WiFi driver and lwIP
run, at priorities in the twenties, and a task at 1 underneath them never gets
the CPU it needs to drain a socket. That measurement is what made the choice
look like "core 1" rather than "a higher priority".

Core 1 at priority 2 improved the median and destroyed the tail. A median
that improves while a twentieth of requests take over half a second is not an
improvement.

Core 0 at priority 10 — above the idle task, below the stack it depends on —
beat the render loop on median, p90 and p99.

### The first comparison was wrong

The measurements above were taken over an afternoon, one configuration at a
time, minutes to hours apart. On that basis the task looked like a disaster:
requests that took 134 ms at worst on the render loop appeared to take three,
five, eleven seconds with the task on, and three consecutive runs of the test
suite took 17, 11 and 73 seconds.

Then the *unmodified* build started showing the same stalls — 1910 ms,
7637 ms, 2540 ms across three consecutive runs of a measurement that had
produced a maximum of 134 ms earlier the same day. The RSSI had drifted from
−48 dBm to −56.

The stalls were the radio, not the code. Comparing A measured at one time
against B measured at another had attributed them to whichever build happened
to be flashed when the air got worse.

### Measured properly, interleaved

Three runs of sixty identical `GET /api/v1/state`, alternating builds back to
back so that both see the same conditions:

| | median | p90 | p99 | max | over 500 ms |
| :--- | ---: | ---: | ---: | ---: | ---: |
| task off | 70 / 71 / 75 | 162 / 164 / 171 | 1807 / 2591 / 2513 | 1910 / 7637 / 2540 | 2, 3, 3 |
| **task on** | 55 / 54 / 62 | 71 / 75 / 81 | **88 / 94 / 118** | **103 / 143 / 147** | **0, 0, 0** |
| task off again | 60 / 64 / 58 | 80 / 178 / 93 | 104 / 355 / 101 | 106 / 1624 / 133 | 0, 1, 0 |

Aggregated: **the task build did not stall once in 180 samples; the render-loop
build stalled 9 times in 360.** The median is better too, by about ten
milliseconds.

There is a mechanism that fits. When the air is poor and a segment needs
retransmitting, the render-loop server can only touch the socket once per
frame — every 32 ms — so recovering from a loss is slow and compounds. A task
servicing it every millisecond handles the same loss promptly. The task helps
*most* exactly when conditions are worst, which is the opposite of what the
first, confounded comparison suggested.

### Verdict: measured over two hours, it is not worth it

`tools/soak.py` was written for this: it builds, flashes and measures both
configurations in every round, reversing the order each round, so both see the
same radio. Forty-five rounds, sixty samples per side per round, two hours.

(It now also records the signal strength on every row, which this run had to
establish by hand afterwards. The conditions a measurement was taken in belong
in the file with it.)

| | p90 | p99 | p99.9 | max | stalls over 500 ms |
| :--- | ---: | ---: | ---: | ---: | ---: |
| task | 105 | 280 | 1640 | 3890 | 14 of 2700 (0.52%) |
| render loop | 105 | 290 | 1360 | 3110 | 12 of 2700 (0.44%) |

The median difference is real and negligible. Paired by round, the task was
faster in 31 of 45 — a sign test gives p = 0.016, so the effect exists — and
it is worth **3.4 ms** on a request of about 58. Six per cent.

Everything else is a tie. p90 is the same number to the millisecond. p99
differs by three per cent. The stall rates, 14 events against 12, are noise.

And the stalls were never the firmware. They appeared in 12 of the 45 rounds
but on *both* sides in only 2, scattered across rounds and sides at random,
which is what an external cause looks like. Round 30: the task build's worst
request was 3890 ms and the render loop's was 287. Round 33: 232 ms and
1977 ms, the other way about. Whichever build happened to be flashed when a
burst of interference arrived wore the stall.

Conditions drifted through the run, as suspected: median 55 ms with 0.27%
stalls in the first half, 64 ms and 0.69% in the second.

So the task is not merged. Three and a half milliseconds does not buy a
threading model, a mutex, a deferred-apply queue, four configuration switches
and four times this project's comment density. It stays on the `http-task`
branch with this page as the reason.

### What was kept

The locking and the deferred applies, which are on `main`. They stand without
the task, because the hazard they close was already there: `setEyeDesign()`
swaps five pointers `drawEye()` dereferences per pixel, and the console could
already call it from `pollCommands()` at an arbitrary point in a frame.
Single-threaded, but not safe. `netShow()` splitting a request from the draw
finishes a discipline the code already half-had in `netShowUntil` and the
splash.

With one thread the rule those impose is one the renderer already followed:
changes land between frames. That costs a reader nothing.

### The lesson worth keeping

All three wrong turns on this page have the same shape. The drain loop was
optimising a mechanism that did not exist; this was measuring two things under
conditions that were not the same. In both cases the code looked plausible and
the first number agreed with the hypothesis.

Interleave the comparison. If A and B cannot be measured within seconds of
each other, the difference between them is not trustworthy -- and a sample of
180 is not a measurement of a 0.5% event. The second wrong turn was reading
"zero stalls in 180 samples" as evidence; at the rate the soak eventually
established, zero in 180 happens about two times in five by chance.

## The open questions, answered

Three questions stood at the end of the work above — where `webPoll()` sits
in the frame, what the remaining outliers were, and whether keep-alive was
worth it. A raw-socket
decomposition -- `tools/test_api.py --decompose N` -- and a plain ping
answered all three, and in the end they had the same answer.

### Where `webPoll()` sits in the frame

Settled by reading rather than measuring. `webPoll()` is near the top of
`frame()` and `drawEye()`, the SPI push, is the last statement in it. The
polls are one frame apart by construction, and there was never anything to
move.

Which leaves the number that prompted the question: a ~35 ms wait against a
32 ms frame, where even spacing should average half of one. That was an
artefact of charging the whole of a request to the board. `urllib` returns one
figure for the exchange; a raw socket can see the seam, because `connect()` is
the handshake by itself and the first byte back cannot arrive until the board
has been round the render loop. 200 requests for `/api/v1/info`, at 34 fps and
-61 dBm:

```
                        median      p90      p99      max
handshake                 25.6     53.6    290.0    558.5
wait + serve              37.8     73.0   2308.6   2442.1
rest of transfer           7.7     42.3   1898.5   2370.2
total                     79.6    150.5   2344.6   2996.6
```

A handshake is a round trip through lwIP's own task, before the sketch has any
say in it -- and its median is 25 ms. So roughly a third of a request is spent
before the render loop is even involved.

The shape of the wait says the rest. There is a hard floor at 15 ms -- not one
wait in 200 came in under it -- and then a flat spread out to about 50 ms.
That is a fixed network leg with one frame of polling on top of it, which is
exactly what the code predicts and nothing more. The poll contributes its half
frame, about 15 ms. It was never the 35.

### The remaining outliers

Confirmed: lost packets. The handshake phase is what proves it, because a slow
`connect()` finishes before the application owns the connection, so the
firmware cannot be the cause. Of 200 requests, six handshakes passed 100 ms:

```
2987, 1064, 242, 143, 111, 110 ms
```

The first three are the shape of exponential backoff -- about 250 ms, then 1 s,
then 1 s + 2 s -- which is what waiting out a retransmission timer looks like.

A ping settles it without any HTTP at all. 100 packets to the board: **6 lost**,
minimum 3 ms, average 27 ms, maximum 1056 ms. The link is quick when a packet
gets through, and the average is inflated entirely by the ones that don't.

The loss is not on the measuring side. This machine is on Ethernet, and loses
0 of 60 packets to the gateway at under 1 ms average. (Another device on the
same access point lost 37 of 60, but a sleeping phone looks like that too, so
take it as nothing more than a hint that the air is busy.)

That also retires the occasional `GET /rtc` at 663 ms. It was not
the I2C read.

### Keep-alive

Worth more than the ten milliseconds it looks like, for a reason easy to
miss. The
handshake is 25 ms of median, not 10 -- but the larger cost is that closing
every connection makes each request carry more packets, on a link that drops
6% of them.

A request with `Connection: close` is about seven packets: SYN, SYN-ACK, the
ACK carrying the request, the reply, and a teardown. At 6% loss, the chance
that a request gets through without waiting on a timer is 0.94^7, about 65%.
On an established connection it is three packets and 0.94^3, about 83%. So
keep-alive would not only save the handshake, it would roughly halve how often
a request hits a retransmission at all. That matches the tail we see: a fifth
of requests running well past a frame.

It is still not a cheap change. This `WebServer` hardcodes the header --
`sendHeader("Connection", "close")` in `_prepareHeader` -- and the code that
would hold a connection open is commented out in the library with a reference
to a Chrome bug. Reaching it means vendoring a core library into `lib/` and
maintaining a fork of it. For 25 ms on a prop, that is the wrong trade today;
it is written down here so the next person does not have to re-derive the
arithmetic before deciding.

### What actually governs responsiveness

In order, which the numbers now put beyond argument:

1. **The radio link.** 6% packet loss at -61 dBm, with a 3 ms floor and a
   1056 ms tail. Every outlier on this page traces back to it. Moving the
   head, or getting its antenna out of the foam, is worth more than any change
   to this firmware -- and costs nothing.
2. **Keep-alive**, for the handshake and the packet count both, at the price
   of forking a library.
3. **The polling interval**, worth about 15 ms on average and bounded below by
   the frame rate. It is the smallest of the three, and the only one that had
   been getting the attention.

Which is the fourth time on this page that the thing being optimised turned
out not to be the thing costing the time. The pattern is consistent enough now
to be the general lesson: measure the phase, not the total.
