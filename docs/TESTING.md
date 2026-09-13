# Testing it, and the tools that come with it

There is a test suite that runs against a real board over HTTP, a soak harness
for comparing two builds over hours, and a handful of scripts that build the
artwork, the page and the firmware stamp.

None of it is needed to use the head. It is here because measuring this
project turned up several things that were not what anyone assumed — see
[HTTP_LATENCY.md](HTTP_LATENCY.md) for where that led.

## Testing it

[`tools/test_api.py`](../tools/test_api.py) exercises a running board over HTTP.
Real hardware, because that is where the interesting failures are — a handler
that works alone but starves the render loop, a value that survives a round
trip but not a reboot, a verb that returns the wrong status only when the body
is malformed.

```sh
python tools/test_api.py --host frank.local
python tools/test_api.py --host 192.168.1.50 --token ...
python tools/test_api.py --host frank.local --user frank --password ...
```

[`tools/soak.py`](../tools/soak.py) is for comparing two builds over hours. It
flashes and measures both in every round, reversing the order each time, so
both see the same radio — measuring one build and then the other compares
their weather, which was enough to invert a conclusion twice before this
existed. See [docs/HTTP_LATENCY.md](HTTP_LATENCY.md).

```sh
python tools/soak.py --hours 3 --a "-DCLOCK=1" --b "-DCLOCK=0"
```

Each row of the CSV carries the signal strength at the time it was taken, and
each round prints it. That column is there because the radio is the variable
that moves on its own: the first comparison run on this project concluded a
change was catastrophically worse, while RSSI drifted from −48 to −56 dBm
underneath it. Timings without the conditions they were taken in are not
evidence.

It reports latency as percentiles and a histogram rather than an average,
because on this board the tail is the interesting part — a mean of 60 ms hides
a request that took eight seconds. `--latency N` skips the tests and times N
requests per endpoint instead, which is how you tell whether a change helped:

```sh
python tools/test_api.py --host frank.local --latency 20
```

`--decompose N` goes one level further and splits each request into its
handshake, its wait, and its transfer, using a raw socket because `urllib`
returns a single number for the whole exchange:

```sh
python tools/test_api.py --host frank.local --decompose 200
```

This is what finally explained where the time goes, and the answer was not the
firmware: on a link losing 6% of its packets, the TCP handshake alone was a
third of a typical request, and every outlier was a retransmission timer. If
the board feels slow, run this before changing any code — see
[docs/HTTP_LATENCY.md](HTTP_LATENCY.md).

Around 117 checks across every endpoint: round trips, range limits, the 400 /
404 / 405 boundaries, malformed bodies, CORS preflight, credentials, and a
burst of gaze updates of the kind dragging the aim pad produces — which
checks both that the last position is the one that sticks and that the eyes
keep rendering while it happens.

It adapts to the firmware it finds: `GET /api/v1/info` says which features are
compiled in, so a build without an RTC or without authentication has those
groups skipped rather than failed. It captures the board's state at the start
and puts it back at the end. Nothing reboots the board or writes flash unless
you pass `--wifi` or `--settings`.

It also reports any request that took over a second, because on this board a
slow request is a stalled render loop.

## The tools

[`tools/ota.py`](../tools/ota.py) uploads firmware over WiFi and verifies it
against the running device,
[`tools/gen_eyes.py`](../tools/gen_eyes.py) converts the artwork,
[`tools/gen_page.py`](../tools/gen_page.py) compresses the control page into the
firmware, and [`tools/git_rev.py`](../tools/git_rev.py) stamps the build with its
commit. [`tools/test_api.py`](../tools/test_api.py) and
[`tools/soak.py`](../tools/soak.py) are described under
[Testing it](#testing-it).

All are type-annotated: every parameter, every return type, and every local
at the point it is introduced. Reassignments carry no annotation, which is
what [PEP 526](https://peps.python.org/pep-0526/) asks for — the name is
declared once, not at every binding — so a count of bare assignments in these
files is not a count of missing types.

All run on Python 3.9, the oldest interpreter PlatformIO is likely to hand
them, and annotations are lazy (`from __future__ import annotations`), so the
modern generic syntax works there too.
