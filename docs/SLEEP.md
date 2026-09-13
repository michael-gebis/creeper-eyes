# Sleep mode

A prop in a bedroom or a hallway does not need to stare all night. Sleep mode
turns the panels dark between two times the user sets, and brings them back in
the morning.

This was written as a design before any of it existed, and is kept that way:
the constraints below are what shaped the result, and several of them are less
convenient than they first look. What was decided, and the one thing the
design did not anticipate, are recorded at the end.

## What "dark" means

Both panels can do this properly, in hardware, rather than by drawing black
pixels. Drawing black would be the obvious approach and the wrong one: it
costs a full SPI push per frame to display nothing, and an OLED showing black
pixels is already dark, so the expensive option buys nothing the cheap one
does not.

| | display off | dim |
| :-- | :-- | :-- |
| SSD1351 (colour) | `SSD1351_CMD_DISPLAYOFF` `0xAE` | `SSD1351_CMD_CONTRASTMASTER` `0xC7`, 0–15 |
| SSD1327 (grey) | `0xAE` | `0x81`, 0–255 |

Both are one command. `0xAF` brings the panel back, and the panel's contents
survive — GDDRAM is untouched by `0xAE`, so waking does not need a redraw,
only the command.

**Two facts worth having before choosing between them.** Neither is in this
document because neither has been measured:

- How much current a panel actually draws with the display off. These modules
  have their own boost converters, and a converter idling is not a converter
  off. `0xAE` may save much less than it sounds like it should.
- Whether "dim" is a useful state at all. At contrast 0 an OLED is very dark
  but not black, which in a dark room may read as a faint glow rather than as
  off. That could be charming or annoying and only looking at it will say.

Both are an evening with a multimeter. Worth doing before committing to a
`level` setting that turns out to have one useful value.

### One API change is needed

`SSD1327::cmd()` and `cmd1()` are **private** (`src/SSD1327.h:124`), so the
grey panel cannot be told anything from outside today. It needs two public
methods — `setPower(bool)` and `setContrast(uint8_t)` — rather than exposing
the raw command interface, which would invite writing arbitrary registers from
anywhere.

The colour side needs nothing: `main.cpp` already calls
`eye[e].display.writeCommand(...)` directly (`src/main.cpp:928`).

## Where the time comes from

`timeLocalSecOfDay(uint32_t &out)` in `src/timekeeping.h` already returns local
seconds-since-midnight with the timezone and DST applied, and — the important
part — **returns false when the time is not known**.

That is the whole guard. A board that has never been told the time, has no
network and no RTC, must not decide it is 3am and go dark. The project already
has this instinct: the clock face hides itself under exactly the same
condition, and the README documents it. Sleep follows the same rule and for
the same reason.

So: if `timeLocalSecOfDay()` returns false, sleep never engages, and the
status says "waiting for the time" rather than "off" — because those are
different states and a user whose eyes failed to sleep deserves to know which
one they are in.

This makes sleep independent of `CLOCK` (the clock face) but dependent on
there being a time source at runtime: NTP, an RTC, or a hand-set time.

## The window

Two settings, `start` and `stop`, as local minutes-since-midnight. The
comparison has to handle the case that matters most, which is the one that
wraps:

```
asleep = (start <= stop) ? (now >= start && now < stop)   // 01:00 -> 06:00
                         : (now >= start || now < stop);  // 22:00 -> 07:00
```

The second line is the normal case for this feature and the first is nearly a
degenerate one, which is a good reason to write both and test both rather than
assume the obvious form.

`start == stop` needs a decision. **Recommendation: it means never sleep**,
not always. A zero-length window reading as "permanently dark" is a way for a
mis-typed field to make the prop look broken, and "never" is the safer
failure.

## Where it sits in the frame

`frame()` already has a precedence chain of things that can own the panels,
and sleep joins the bottom of it:

```
src/main.cpp
  2041  webPoll()                  <- everything above the returns keeps running
  2094  fps accounting
  2286  if (webRebootPending())    return;   an update is about to reboot
  2290  if (netShowUntil)          return;   address cards
  2298  if (splashPoll())          return;   startup cards
        ---> sleep goes here <---
  2305  frames++
  2307  drawEye(...)
```

Below the address cards and the splash deliberately: someone who presses
"show address" at 3am wants the address, and an update landing overnight
should still say so on the panels. Sleep is the lowest-priority claim on the
displays, not the highest.

**`webPoll()` is at the top, well above every early return**, so a sleeping
board still serves the web UI and the API at full speed. That is not an
accident of this design; it is why the web server is polled where it is. Worth
stating because the first question anyone will ask is whether the thing
becomes unreachable at night.

### The consequence nobody will expect

`frames++` sits below the return, so a sleeping board reports **fps = 0**.

That is arguably correct — nothing is being rendered — but this project has
already shipped one bug where `system.fps` read zero and looked broken
(`239d0a0`). Reintroducing a zero, even a legitimate one, needs to be
deliberate and visible: the API should report `sleeping: true` alongside it,
and the page should say "asleep" where it would otherwise show a frame rate.
A number that is zero for a good reason still needs the reason attached.

## Waking

This is the real design question, and it is a matter of taste rather than
engineering. Three options:

1. **Sleep is sleep.** The window is the window; nothing wakes it early. The
   simplest to build and to explain, and the most likely to annoy someone who
   walks past at midnight and wants to show the thing off.
2. **Any command wakes it, until the window ends.** A gaze change, an eye
   change, anything through the API or console lights it up and it stays up
   until morning. Simple rule, easy to describe, but one stray poll from a
   script leaves the eyes on all night.
3. **A command wakes it for a while** — say sixty seconds, then back to sleep
   if the window is still open. Matches what a phone screen does, and matches
   `netShowUntil`, which this codebase already uses for exactly this shape of
   behaviour ("hold the panels until a deadline, then let go").

**Recommendation: 3**, with the duration as a setting and 0 meaning option 1.
It reuses a pattern that is already here, and the failure mode — eyes on for a
minute at 2am — is mild in both directions.

Option 2 is worth rejecting explicitly: the web page polls every second while
it is open, so "any request wakes it" would mean a forgotten browser tab keeps
the prop awake all night. Whatever the rule is, **the page's own polling must
not count as interaction.** Only state-changing requests should.

## Configuration

Following the project's existing patterns rather than inventing new ones.

**Compile-time**, in `config.h`, all `#ifndef`-guarded:

| | default | |
| :-- | :-- | :-- |
| `SLEEP` | `1` | The whole feature. `0` compiles it out entirely. |
| `SLEEP_START_MIN` | `22 * 60` | Built-in default, overridden by the stored value |
| `SLEEP_STOP_MIN` | `7 * 60` | |
| `SLEEP_WAKE_S` | `60` | How long interaction holds it awake; 0 for never |

**Stored**, in the existing `creeper` NVS namespace alongside the clock
settings, so `save`/`forget` and the factory reset already cover it: enabled,
start, stop, level.

**API**, `GET`/`PUT /api/v1/sleep`:

```json
{"enabled": true, "start": "22:00", "stop": "07:00",
 "level": 0, "asleep": false, "reason": "the time is not known yet"}
```

`start`/`stop` as `"HH:MM"` strings rather than minute counts — the page needs
`<input type=time>` to produce exactly that, and a REST API that a human can
drive with `curl` is worth more here than two fewer string parses.

`asleep` and `reason` are read-only and are what the UI renders. `reason`
carries the case above: enabled, but no time source, so not sleeping.

**Page**: one card, tagged `persistent`, with an enable toggle, two time
inputs, and a line reading "asleep until 07:00" or "awake — sleeps at 22:00"
or "waiting for the time". Consistent with how the clock card already reads.

**Console**: `sleep` to show, `sleep on|off`, `sleep 22:00 07:00`, matching the
shape of the existing commands.

## Edge cases worth writing tests for

- **The window wraps midnight.** The main case, and the one a naive
  implementation gets wrong.
- **The time jumps.** An NTP sync landing at 02:00 can move the clock across a
  boundary in one step. Since the window is recomputed every frame from the
  current time, this resolves itself — but it means sleep must never latch a
  decision it made earlier.
- **A DST transition inside the window.** `timeLocalSecOfDay()` handles it;
  the test is that nothing else caches a local time across it.
- **Sleep engaging while the address cards are up.** The cards win until their
  deadline, then sleep takes over. Worth an explicit test because the
  precedence is easy to invert by accident.
- **Waking to draw.** If `0xAE` is used, *anything* that wants the panels —
  `showMessage()`, `netShow()`, the splash, an OTA progress card — has to turn
  them back on first. That argues for a single `displaySleep(bool)` helper
  that owns the command and a `displayWake()` that those callers go through,
  rather than scattering `0xAF` around. **This is the part most likely to
  produce a bug that only shows up at 3am**, which is the worst time to
  discover one.

## What this is not

Not a power-saving feature for the ESP32. The radio, the CPU and the web
server all keep running; only the panels go dark. Deep sleep on the ESP32
would mean the board stops answering, which is a different feature with a
different set of consequences, and is not this one.

Not a brightness schedule. One window, two times. A general-purpose "dim at
dusk, brighter at noon" curve is a larger thing, and if it is ever wanted it
should replace this rather than grow out of it.

## What was decided, and what it became

All three questions went the way the recommendations pointed.

1. **Both, with off as the default.** `SLEEP_LEVEL` is 0, which switches the
   panels off and stops rendering. Any value from 1 to 100 dims them instead
   and *keeps drawing*, so the eyes still move faintly — a nightlight rather
   than a sleep. Freezing the last frame at low contrast was the third option
   and would have looked like a fault rather than a setting.
2. **Awake for a minute after anything deliberate.** `SLEEP_WAKE_S` is 60, and
   0 makes the window absolute. The nudge is called from three places, each a
   real boundary rather than a sprinkling through the operations layer:
   `handleCommand()` (which `/cmd` also lands in), the BOOT button, and
   `guarded<>` in `api.cpp` for any method that is not GET or OPTIONS. That
   last condition is the important one — the page polls once a second, so
   counting reads would have meant a forgotten browser tab kept the head awake
   all night.
3. **`start == stop` means never.**

### One thing the design did not have

`GET /api/v1/sleep` reports `now`, the board's own local time of day — the
very value the window is judged against.

It was added while writing the tests, which needed to know what time the board
thought it was in order to place a window around it. The obvious sources were
both wrong: `time` carries no local wall clock, and `clock.secondOfDay` can be
running at an accelerated `rate` for testing, which would have put every test
window in the wrong place and produced failures that looked like sleep bugs.

Reporting the number the decision is made from removes the guesswork for the
page too, which can now say what the board thinks the time is at the moment
that is the thing in doubt.

### The 3am bug, headed off

The design flagged this as the part most likely to break in the dark: with the
panels switched off, *anything* that wants to draw has to turn them back on
first, or an address card at 3am arrives invisibly.

It is handled in one place. `pushCanvas()` — which every card in the firmware
goes through, from the splash to the address cards to the OTA progress
display — calls `displaySetPower(true)` before it draws. `displaySetPower()`
tracks the current state and returns immediately when it is already right, so
callers ask freely and nothing pays for asking. Sleep takes the panels back on
the next frame after the card's deadline passes.

That is why sleep sits *below* the card checks in `frame()` rather than above
them: the cards return before `sleepPoll()` is ever reached, so the two never
argue over the same frame.
