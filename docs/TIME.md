# Keeping time

Frank can tell the time three ways, and prefers them in this order: a time
server, a time you typed in, then a battery-backed clock chip — somebody
correcting the clock by hand means it, so that outranks a time restored from
the chip at boot. With none of
them it knows it does not know, and says so rather than guessing — the clock
face hides itself instead of showing a confident lie.

The [clock face](#clock-face) is the visible half of this: an analogue clock
drawn in the iris. The rest is about where the hands get their time from.

## Time

Taken from NTP once connected. The timezone is a POSIX string, which carries
the DST **rules** rather than a fixed offset, so the changeover happens by
itself:

```
> tz pacific
ok tz=PST8PDT,M3.2.0/2,M11.1.0/2
> save
```

`tz` with no argument lists the sixty-odd named zones, grouped by region:
they are IANA city names — `los_angeles`, `kolkata`, `auckland`, `kathmandu`
— so the one you want is the one you would guess. The regional names this
project started with (`pacific`, `eastern`, `uk`, …) still work.

Anywhere not on the list works too: `tz` takes a raw POSIX string, which is
what the C library wants in the end. The full IANA database is megabytes and
needs a filesystem; a POSIX string is thirty bytes, and the trade is that a
country changing its DST rules needs a firmware update rather than a data
one. For a Halloween prop that is the right side of the deal.

### Reading a POSIX timezone string

If you have only ever seen `PST` or `GMT`, the strings above look like line
noise. They are not an abbreviation — they are a complete daylight-saving
rule, which is why the board can handle the spring and autumn changeovers
without being told about them or being online at the time.

Taking `PST8PDT,M3.2.0/2,M11.1.0/2` apart:

| | |
| :-- | :-- |
| `PST` | what to call the time when DST is not in effect |
| `8` | hours **behind** UTC — see the sign trap below |
| `PDT` | what to call it when DST *is* in effect |
| `M3.2.0/2` | DST **starts**: month 3, week 2, day 0 (Sunday), at 02:00 |
| `M11.1.0/2` | DST **ends**: month 11, week 1, Sunday, at 02:00 |

So: US Pacific, starting DST on the second Sunday in March and ending it on
the first Sunday in November. In `Mm.w.d`, the day is 0–6 with Sunday as 0,
and the week is 1–5 where **5 means "the last one in the month"** rather than
literally the fifth. That is how "the last Sunday in October" gets written,
and it is what the built-in `london` uses: `GMT0BST,M3.5.0/1,M10.5.0/2` —
last Sunday in March at 01:00, last Sunday in October at 02:00.
The offset after the DST name is optional and defaults to one hour ahead of
standard time, which is why `PDT` carries no number here.

**The sign is backwards from what you expect.** The number is positive when
you are *west* of Greenwich, so US Pacific is `8`, not `-8`. This is the
opposite of the convention used by almost everything else, including UTC±
offsets as people normally write them. Getting it wrong puts the clock out by
twice the offset, which at least makes the mistake obvious.

Zones with no daylight saving are just the first two fields: `IST-5:30` for
India, where the offset is negative because it is east, and minutes are
allowed.

The authoritative description of the format — every field, every edge case —
is the GNU C Library manual's
[Proleptic Format for TZ](https://sourceware.org/glibc/manual/latest/html_node/Proleptic-TZ.html).
That page is the one to read if you are constructing a string for somewhere
not in the built-in list.

Defaults to US Pacific, and is persisted.

Once time is synced, `clock rate` stops having any effect: it drives the
free-running fallback, which is no longer what feeds the hands. `clock set`
still works — it outranks a time restored from the RTC, on the grounds that
somebody correcting the clock by hand means it — but NTP outranks it in turn.

There are up to four sources, ranked, and a better one is never overridden by
a worse one:

| | Source | Set by |
| :-- | :--- | :--- |
| lowest | free-running | boots at 10:10 and drifts; the face stays hidden |
| | the RTC | read once at boot, if one is fitted |
| | set by hand | `clock set` |
| highest | NTP | a time server answering; also writes the RTC |

`clock`, `tz`, the control page and `GET /api/v1/state` all report which one
is in charge, as `time.source`.

The control page shows the state of both time sources — whether NTP is built
in, has a link, and when it last heard back; whether an RTC is built in,
present, and holding a time worth believing — and carries a **sync now**
button, which restarts the client so it asks immediately instead of waiting
out the three hours.

NTP can also be switched off, from the page or with `ntp off`. It is a saved
setting. The clock keeps whatever the server last gave it, but stops being
defended by it, so setting the time by hand afterwards works — which it does
not while a server is in charge. The page hides the manual time field
whenever NTP is on, rather than offering a control that would accept a value
and then have no effect.

## Keeping time without a network

Fit a [DS3231](WIRING_RTC.md) and build with `-DRTC=1`. Set the timezone
and the time once, and the head keeps it — through power cuts, and with no
network ever configured:

```
> tz chicago
> clock set 16:34
> save
```

The chip holds **UTC**, and the timezone is applied on the way out, so a head
unplugged in February and switched on in July still shows the right hour.
That is also why `tz` works in no-network builds: it is a saved setting like
any other now, not part of the networking.

With a network as well, the first NTP sync writes the chip by itself, so the
time is right immediately at the next boot rather than a few seconds later.

## Clock face

**Experimental.** Turns the iris into an analogue clock with hour, minute and
optional second hands.

```
> clock on
> clock set 10:10
> clock rate 600           # 10 minutes of clock per second
> clock color sec FF8800   # a pop colour on the second hand
```

With no time source at all the clock free-runs from `millis()` off a
time you set. `clock rate` exists because at 1× you cannot tell whether the
hour hand works without waiting an hour.

Hands are drawn **after** the eye is rendered, straight into the finished
frame, as filled quads of constant pixel width. Two things follow from that:

- They sit on top of whatever is underneath, so they work with or without a
  pupil. Black hands read as a silhouette on a light iris but vanish over the
  black pupil, which is what the colour command is for.
- The iris-circle and eyelid clips the pixel loop would have provided are
  applied explicitly, so hands stop at the iris edge and disappear properly
  behind a blink.

The eyes keep wandering and blinking while the clock runs, so it drifts around
and gets blinked away. `look 512 512` pins the gaze if you want it readable —
though the wandering version is arguably the creepier one.

Set `CLOCK` to 0 in [`src/config.h`](../src/config.h), or pass
`-DCLOCK=0` in `build_flags`, to compile the whole feature out.
