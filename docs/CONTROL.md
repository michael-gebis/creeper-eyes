# Driving the head

Three ways to take over from the autonomous motion, all reaching the same
operations underneath: a web page, a serial console, and a REST API. Anything
one can do, the others can too.

If you are not sure which you want: open `http://frank.local/` in a browser.

## Web interface

![The control page](images/webui.png)

**http://frank.local/** — a control page for everything the console can do:
eye design, gaze, dilation, pupil, panel swap, clock and hand colours, the
time and its sources, sleep, passwords, Wi-Fi, and the address details. It polls the device once a second,
so two browsers looking at it stay in step with each other and with anything
you type over serial.

Each card says what happens to its settings when the power goes off —
persistent, session only, or momentary — because that is the first question
anyone asks of a control they have just moved.

The page is static: [`data/index.html`](../data/index.html), gzipped into the
firmware at build time by [`tools/gen_page.py`](../tools/gen_page.py) and served
straight out of flash. 36 KB becomes 12, which took the page load from 554 ms
to under 100 — the board sends roughly one TCP segment per rendered frame, so
the only thing that really helps is sending fewer of them. Its tab icon is an inline SVG `data:` URI from
[`src/favicon.h`](../src/favicon.h) rather than a `/favicon.ico` route — no
second handler, and no second request against a server that manages one
client at a time. Two are bundled: Frank's head, and just the eyes for a
build going into something that is not a Frankenstein. Pick with `FAVICON`. Everything on it is drawn from the API below, so there
is no markup anywhere that has to be kept in step with device state.

Requests are served from the render loop, which is also the floor on how fast
they can be: about 50 ms, almost none of it the handler. There are
measurements and the reasoning in
[docs/HTTP_LATENCY.md](HTTP_LATENCY.md), including one optimisation that
turned out not to work.

Each request costs a dropped frame or two. That is why the page polls at a leisurely rate and the responses are
kept small. The page schedules its next poll when the last one lands rather
than on a timer: the board serves one client at a time, so a timer would
leave requests outstanding behind each other until the browser ran out of
connections and the page stopped responding.

WiFi modem sleep is turned off for the same reason. The default parks the
radio between beacons, which measured at a **1.7 s median** for one small
`GET`, with a tenth of them past eight seconds; with it off the same request
takes **65 ms** and none time out. It costs roughly 30 mA, which is nothing
for a prop on a USB lead.

## Serial console

Open `pio device monitor` and type `help`. Commands are line-based at 115200.

| Command | Effect |
| :------ | :----- |
| `eye` | List the eye designs built into this firmware |
| `eye <name>` | Select a design by name, e.g. `eye newt` |
| `eye <index>` | Select by number, e.g. `eye 1` |
| `eye next` | Cycle to the next design |
| `look <x> <y>` | Aim the gaze; each 0–1023, `512 512` is centre |
| `look auto` | Hand gaze back to autonomous motion |
| `dilate <0-100>` | Pupil width; `100` is fully dilated |
| `dilate auto` | Hand dilation back to autonomous |
| `startle` | Constrict slowly, then snap wide with a blink |
| `pupil [on\|off]` | Pupil, or a full iris disc |
| `clock [on\|off]` | Analogue clock in the iris |
| `clock set HH:MM[:SS]` | Set the time |
| `clock rate <1-3600>` | Run the clock faster, for testing |
| `clock secs [on\|off]` | Show or hide the second hand |
| `clock color [hour\|min\|sec] RRGGBB` | Hand colours |
| `blink` | Blink both eyes now |
| `swap [on\|off]` | Swap which physical panel is which eye |
| `save` | Persist every setting tagged persistent — see [Configuring](CONFIG.md#remembering-settings) |
| `forget` | Clear saved settings |
| `net [quiet]` | Address info, on the panels too — one eye shows a code that opens this page |
| `net off` | Dismiss the address cards early |
| `wifi` | The network, and how to change it |
| `wifi join <ssid> [pass]` | Store a network and reboot into it |
| `wifi forget` | Clear the stored network |
| `wifi portal` | Reboot into the setup portal |
| `version` | Firmware version, commit and build date |
| `ntp [on\|off\|sync]` | Use a time server, stop using one, or ask again now |
| `rtc` | Battery-backed clock: present, valid, its time and temperature |
| `rtc sync` | Store the current time in it |
| `tz [zone]` | Timezone by name or POSIX string |
| `splash` | Re-show the panel name cards |
| `status` | Current eye, gaze, dilation, heap, uptime, frame rate |
| `sleep` | What the sleep window is set to, and what it is doing |
| `sleep on\|off` | Enable or disable it |
| `sleep HH:MM HH:MM` | The window: when to sleep, then when to wake |
| `sleep level <0-100>` | `0` switches the panels off; above that, dims them |
| `help` | The list above. `?` does the same |

The **BOOT button** toggles the eye artwork, which is handy on the bench but
unreachable once the head is assembled — hence the console.

Overrides are sticky: `look` and `dilate` hold their commanded value until you
return them with `auto`. The autonomous animation keeps running underneath, so
handing control back is seamless.

## REST API

Everything the page does, `curl` can do. **`/api/v1`**, JSON in and JSON out,
CORS open so a page served from anywhere can drive the device.

| Method | Path | What it does |
| :----- | :--- | :----------- |
| `GET` | `/api/v1/state` | Everything at once — what the page polls |
| `GET` | `/api/v1/eyes` | The eye designs this firmware was built with |
| `GET` | `/api/v1/net` | MAC, addresses, signal, sync state |
| `GET` | `/api/v1/info` | Version, commit, build date, project URL, whether a credential is needed |
| `GET` `PUT` | `/api/v1/ntp` | Time-client status; `{"enabled":false}` stops it, `{"op":"sync"}` asks now |
| `GET` `PUT` | `/api/v1/rtc` | The battery-backed clock; `{"op":"sync"}` stores the time. Only with `RTC=1` |
| `GET` `PUT` | `/api/v1/eye` | `{"name":"dragon"}`, `{"index":2}` or `{"next":true}` |
| `GET` `PUT` | `/api/v1/gaze` | `{"x":200,"y":800}` or `{"mode":"auto"}` |
| `GET` `PUT` | `/api/v1/dilate` | `{"percent":40}` or `{"mode":"auto"}` |
| `GET` `PUT` | `/api/v1/pupil` | `{"on":false}` |
| `GET` `PUT` | `/api/v1/swap` | `{"on":true}` — swaps left and right panels |
| `GET` `PUT` | `/api/v1/clock` | `on`, `seconds`, `rate`, `time`, `colors` — any subset |
| `GET` `PUT` | `/api/v1/netinfo` | `{"on":true}` — address cards on the panels |
| `GET` `PUT` | `/api/v1/wifi` | `{"ssid":…,"pass":…}`, `{"op":"forget"}`, `{"op":"portal"}` |
| `GET` `PUT` | `/api/v1/tz` | `{"tz":"pacific"}` or any POSIX string |
| `POST` | `/api/v1/action` | `{"action":"blink"}` — also `startle`, `splash`, `netinfo` |
| `POST` | `/api/v1/settings` | `{"op":"save"}` or `{"op":"forget"}` |
| `GET` `PUT` | `/api/v1/sleep` | `enabled`, `start`, `stop`, `level` — needs `SLEEP` |
| `GET` `PUT` | `/api/v1/credentials` | Which credentials are set, and changing them — see below |

```sh
# A body must be sent as JSON -- curl defaults to form encoding, which the
# ESP32 web server consumes before a handler ever sees it.
alias frank='curl -sH "Content-Type: application/json" http://frank.local/api/v1'

frank/state
frank/eye    -X PUT  -d '{"name":"dragon"}'
frank/gaze   -X PUT  -d '{"x":200,"y":800}'
frank/clock  -X PUT  -d '{"on":true,"colors":{"second":"FF8800"}}'
frank/action -X POST -d '{"action":"startle"}'
```

A `PUT` returns the resource as it now stands, so there is no need to `GET`
afterwards to find out what happened. Failures carry a reason:

```json
{"error": "x and y must each be 0-1023"}
```

`400` for a bad body or an out-of-range value, `404` for an eye design this
build does not contain or an RTC it does not have, `405` for the wrong verb on
a real path, `409` where NTP and a hand-set time would conflict, and `500` if
storage fails. With authentication compiled in, `401` for a missing or wrong
credential and `403` for a `Host` that is not this device — or for changing a
credential without presenting the current one.

Every write to `/wifi` answers first and then reboots the board, so the reply
arrives but the connection it arrived over does not survive. `GET /wifi`
never returns a password.

Gaze runs `0`–`1023` on each axis with **`y=1023` at the top**, the way a
joystick reads rather than the way a screen does. `512 512` is centre. The
control page flips it so that dragging up looks up.

Authentication is off by default, and optional at build time: with the
switches off none of it is compiled in, and anything that can reach the board
can drive it. That is the right trade for a prop on a home network and the
wrong one anywhere else — see [Locking it down](SECURITY.md) for the three
mechanisms and how to turn them on.

## The `/cmd` escape hatch

For anything the API does not model yet, `/cmd?c=<command>` hands a line
straight to the console's dispatcher:

```sh
curl "http://frank.local/cmd?c=help"
curl "http://frank.local/cmd?c=status"
```

It returns plain text, not JSON, and it is a convenience rather than an
interface — prefer the API for anything you are writing against. Set
`WEB_CMD_ENDPOINT` to `0` to leave it out.
