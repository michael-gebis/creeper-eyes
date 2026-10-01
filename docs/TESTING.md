# Testing it, and the tools that come with it

There is a test suite that runs against a real board over HTTP, a soak harness
for comparing two builds over hours, a load test that catches a board
restarting, and a handful of scripts that build the artwork, the page and the
firmware stamp.

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
uv run tools/test_api.py --host frank.local
uv run tools/test_api.py --host 192.168.1.50 --token ...
uv run tools/test_api.py --host frank.local --user frank --password ...
```

[`tools/soak.py`](../tools/soak.py) is for comparing two builds over hours. It
flashes and measures both in every round, reversing the order each time, so
both see the same radio — measuring one build and then the other compares
their weather, which was enough to invert a conclusion twice before this
existed. See [docs/HTTP_LATENCY.md](HTTP_LATENCY.md).

```sh
uv run tools/soak.py --hours 3 --a=-DCLOCK=1 --b=-DCLOCK=0
```

The `=` matters: written as `--a -DCLOCK=1`, the flag would be taken for
another option and the command refused.

Each row of the CSV carries the signal strength at the time it was taken, and
each round prints it. That column is there because the radio is the variable
that moves on its own: the first comparison run on this project concluded a
change was catastrophically worse, while RSSI drifted from −48 to −56 dBm
underneath it. Timings without the conditions they were taken in are not
evidence.

[`tools/load_test.py`](../tools/load_test.py) is for power. It runs the suite
against one board over and over and reports every time the board restarted,
and why. A board on a marginal supply browns out when its current steps up,
and the biggest step it takes is the eyes coming back to full speed after a
pause — which an eye-file upload makes, stalling both cores while flash is
written. So pass one through to the suite:

```sh
uv run tools/load_test.py --host frank.local --minutes 45 --port COM3 \
    -- --eye-file dist/eyes/dragon.bin
```

It notices a restart two ways. Between runs, an uptime that has fallen behind
the clock means the board restarted, and its warnings then name the reason —
a brownout, a crash, a watchdog. With `--port` it also watches the serial
port, without resetting the board, and prints what led up to each restart.
Everything after `--` goes to `test_api.py` unchanged; like the suite, it
leaves the eye file loaded. The numbers in
[Frame rate](FRAME_RATE.md#brownouts-and-the-cpu-at-160-mhz) came from it.

It reports latency as percentiles and a histogram rather than an average,
because on this board the tail is the interesting part — a mean of 60 ms hides
a request that took eight seconds. `--latency N` skips the tests and times N
requests per endpoint instead, which is how you tell whether a change helped:

```sh
uv run tools/test_api.py --host frank.local --latency 20
```

`--decompose N` goes one level further and splits each request into its
handshake, its wait, and its transfer, using a raw socket because `urllib`
returns a single number for the whole exchange:

```sh
uv run tools/test_api.py --host frank.local --decompose 200
```

This is what finally explained where the time goes, and the answer was not the
firmware: on a link losing 6% of its packets, the TCP handshake alone was a
third of a typical request, and every outlier was a retransmission timer. If
the board feels slow, run this before changing any code — see
[docs/HTTP_LATENCY.md](HTTP_LATENCY.md).

Around 200 checks across every endpoint: round trips, range limits, the 400 /
404 / 405 boundaries, malformed bodies, CORS preflight, credentials, brightness
and its fades, and a burst of gaze updates of the kind dragging the aim pad produces — which
checks both that the last position is the one that sticks and that the eyes
keep rendering while it happens.

It adapts to the firmware it finds: `GET /api/v1/info` says which features are
compiled in, so a build without an RTC or without authentication has those
groups skipped rather than failed. It captures the board's state at the start
and puts it back at the end. Nothing reboots the board or writes flash unless
you pass `--wifi`, `--settings` or `--restart`. The same goes for `--eye-file
dist/eyes/NAME.bin`, which fills the [eye slot](EYE_FILES.md), damages an
upload on purpose, and removes it again. The uploads the board refuses
*before* erasing anything are tested on every run, and checked to have left
the slot alone.

`--restart` restarts the board twice: once to move the CPU speed to its other
setting, and once to move it back. It checks that the new speed took, and that
a restart the board was asked for leaves no warning behind.

A group called *refusing bad input* sends the API the kinds of values it
once accepted wrongly: out-of-range numbers that a narrowing cast used to
wrap into range, times with trailing junk, fields of the wrong type, and
one bad field beside a good one, which must leave the good one unapplied
too. None of it changes the board, including the WiFi cases, each of which
would reboot it if it were ever accepted.

A group called *taking a request* sends four requests `urllib` will not
send:

- a head too large, which must get `431`;
- a body too large, which must get `413`;
- a broken `Content-Length`, which must get `400`;
- a request that never finishes arriving, which must be dropped. While it
  waits, the frame rate is checked to show the eyes kept moving.

That last one costs five seconds a run.

It also reports any request that took over a second, because on this board a
slow request is a stalled render loop.

## The tools

[`tools/ota.py`](../tools/ota.py) uploads firmware over WiFi and verifies it
against the running device,
[`tools/gen_eyes.py`](../tools/gen_eyes.py) converts the artwork,
[`tools/make_eye.py`](../tools/make_eye.py) turns a design into an
[eye file](EYE_FILES.md),
[`tools/gen_page.py`](../tools/gen_page.py) compresses the control page into the
firmware, [`tools/git_rev.py`](../tools/git_rev.py) stamps the build with its
commit, and [`tools/nvs_backup.py`](../tools/nvs_backup.py) saves everything the
board remembers to a JSON file and puts it back — including the WiFi network,
which the radio keeps in a namespace of its own. That one is USB only, and
[Configuring](CONFIG.md) explains what is in there. [`tools/test_api.py`](../tools/test_api.py),
[`tools/soak.py`](../tools/soak.py) and [`tools/load_test.py`](../tools/load_test.py) are described under
[Testing it](#testing-it).

All are type-annotated: every parameter, every return type, and every local
at the point it is introduced. A count of bare assignments in these files is
not a count of missing types, for three reasons. Reassignments carry no
annotation, which is what [PEP 526](https://peps.python.org/pep-0526/) asks
for — the name is declared once, not at every binding. Tuple unpacking
(`code, raw = api.raw(...)`) and the targets of `with` and `for` cannot carry
one syntactically; every such binding here takes its type from an annotated
call or a literal, and where neither applies — the fields `struct.unpack`
returns, say — the names are declared on their own lines just above. And `Any` appears where it is honest to — decoded JSON and
parsed NVS values really are of unknown type, and claiming otherwise would
be worse than saying so.

### Running them

The tools run on Python 3.14 through [uv](https://docs.astral.sh/uv/), which
the repository is set up for. `pyproject.toml` names the three packages they
need — Pillow for `gen_eyes.py`, pyserial for `load_test.py --port`, esptool
for `nvs_backup.py` — and `uv.lock` pins them. `uv run tools/<script>.py`
fetches Python and the packages the first time, and runs the script; nothing
is installed anywhere else.

uv keeps that environment in `.venv` inside the repository unless
`UV_PROJECT_ENVIRONMENT` points elsewhere. Point it elsewhere if the
repository lives in a synced folder — Google Drive, Dropbox, OneDrive — or
the environment is uploaded along with it:

```sh
export UV_PROJECT_ENVIRONMENT=~/.cache/uv-envs/creeper-eyes
```

(`$env:UV_PROJECT_ENVIRONMENT = "..."` in PowerShell.)

Four scripts are not run that way. [`tools/git_rev.py`](../tools/git_rev.py)
and [`tools/gen_page.py`](../tools/gen_page.py) are PlatformIO build hooks, run
by whatever Python PlatformIO itself runs on — 3.14 for a
`uv tool install platformio`, 3.11 inside the VS Code extension — so they use
the standard library only, and no syntax newer than 3.11.
[`hardware/gen_board.py`](../hardware/gen_board.py) and
`hardware/make_outputs.py` need KiCad's `pcbnew` and run on the Python inside
KiCad; the [hardware README](../hardware/README.md) says how.
