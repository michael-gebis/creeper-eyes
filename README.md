# Frankenstein Head with Animated Eyes

Twin animated eyes for the 3D printed [Frankenstein head with animated
eyes](https://www.printables.com/model/620191-frankenstein-head-with-animated-eyes),
driven by an ESP32 and two 1.5" OLED panels.

The eyes wander, blink and dilate on their own. A serial console lets you take
over — aim the gaze, set the pupil, swap the eye artwork, or fire a startle
effect — without opening the head up.

## What this fork adds

- **Runs on a generic ESP32 dev board**, not just the Adafruit Feather
- **Two panel types supported** — SSD1351 colour and SSD1327 grayscale
- **Serial command console** over the USB cable that already powers the board
- **Analogue clock face** rendered in the iris, on real time from NTP
- **WiFi** with a setup portal, reachable at `frank.local`
- **Web control page** for every eye, gaze, pupil and clock setting
- **JSON REST API** at `/api/v1`, so other programs can drive Frank too
- **Over-the-air updates**, so a sealed head never needs opening
- **Startup splash** naming each panel, so you never have to trace wires
- **BOOT button** toggles between the two eye designs
- **25 eye designs** to choose from, selected at build time
- **Display diagnostics** for bringing up new hardware
- Fixes for three rendering and initialisation bugs — see [Credits](#credits)

## What to buy

### ESP32 board

Any **classic ESP32** (ESP-WROOM-32 module, Xtensa dual core) with 4 MB flash.

> **Not** an ESP32-**S2**, **S3**, **C3** or **C6**. Those are different
> architectures and will not run this build. If your board shows up as USB
> vendor ID `303A`, it is one of those. A classic ESP32 uses a separate
> USB-serial chip — CH340 (`1A86`) or CP210x (`10C4`).

Known good: the 30-pin **DOIT ESP32 DevKit V1**, sold under a dozen names with
"NodeMCU", "ESP32S" and "Type-C" in the title. Roughly $5 each in three-packs.
Either USB-serial chip is fine; CH340 may need a driver on Windows.

### Displays — read this part carefully

Waveshare sells **two different 1.5" 128×128 OLED modules** with nearly
identical names and **identical 7-pin headers**. You cannot tell them apart by
wiring, and the wrong one shows white static.

| Product | Controller | Display | Build to use |
| :------ | :--------- | :------ | :----------- |
| **1.5inch RGB OLED Module** | SSD1351 | 65K colour | `esp32dev` |
| **1.5inch OLED Module** | SSD1327 | 16 greys | `gray` |

Buy **two**. The **RGB** version is what the eye artwork was drawn for — the
hazel and newt irises carry most of their detail in hue, and grayscale
flattens them. The grayscale version works and runs faster, but the eyes lose
a lot of their character.

Both are 3.3 V / 5 V tolerant and need no level shifting.

### Everything else

- Breadboard and male-to-female jumper wires (14 connections)
- USB-C cable — this also carries the serial console
- The printed parts and hardware from the
  [Printables model](https://www.printables.com/model/620191-frankenstein-head-with-animated-eyes)

A plain USB port powers everything comfortably. Two panels plus an idle ESP32
draw well under 200 mA.

## Wiring

Six signals are shared by both panels; only chip select differs. Left and
right below are **Frank's own**, so facing him, his right eye is the one on
your left.

| Signal | DevKit pin | Frank's right (your left) | Frank's left (your right) |
| :----- | :--------- | :------------------------ | :------------------------ |
| VCC    | `3V3`      | VCC | VCC |
| GND    | `GND`      | GND | GND |
| DIN    | `D18`      | DIN | DIN |
| CLK    | `D5`       | CLK | CLK |
| DC     | `D33`      | DC  | DC  |
| RST    | `D27`      | RST | RST |
| CS     | `D15` / `D4` | CS ← `D15` | CS ← `D4` |

> **Use `3V3`, never `VIN`.** ESP32 GPIOs are 3.3 V and not 5 V tolerant.

Module silkscreens vary between sellers — `DIN` may be labelled `SDA`, `SI` or
`MOSI`; `CLK` may be `SCL`, `SCK` or `D0`. Match by position, not by letters.

**Full details, assembly order and troubleshooting: [docs/WIRING.md](docs/WIRING.md).**

## Build and flash

This is a [PlatformIO](https://platformio.org/) project. No Arduino IDE needed.

```sh
pipx install platformio          # or: pip install --user platformio
```

Everything else — the Xtensa toolchain, the ESP32 Arduino core, the Adafruit
libraries — downloads automatically on the first build.

### Environments

| Environment | Builds | Use for |
| :---------- | :----- | :------ |
| `esp32dev` | Eyes, SSD1351 colour | **Default.** Waveshare 1.5inch RGB OLED |
| `gray` | Eyes, SSD1327 grayscale | Waveshare 1.5inch OLED |
| `displaytest` | Solid-colour fills, SSD1351 | Bring-up and fault isolation |
| `probe1327` | Raw SSD1327 init, no library | Identifying an unknown panel |
| `esp32dev_ota` / `gray_ota` | Same firmware, flashed over WiFi | Updating a sealed head |

### Commands

```sh
pio run                              # build the default (colour) environment
pio run -e gray -t upload            # build and flash the grayscale build
pio run -e gray -t upload -t monitor # ...and open the console
pio device monitor                   # console only
pio device list                      # find the serial port
```

If the port is picked wrongly, pin it in `platformio.ini` with
`upload_port = COM5` (or `/dev/ttyUSB0`).

## Build options

Compile-time switches. All of them live in [`src/config.h`](src/config.h),
and each is `#ifndef`-guarded, so any of them can also be overridden from
`build_flags` in `platformio.ini` without editing a source file.

| Option | Default | Effect |
| :----- | :------ | :----- |
| `USE_SSD1327` | `0` | Selects the grayscale panel driver. Set by the `gray` environment via `build_flags`, not edited by hand. |
| `COMMANDS` | `1` | Serial console and BOOT-button toggle. `0` compiles both out; the REST API still works, so a network build stays fully controllable. |
| `DEBUG` | `1` | Serial diagnostics and the 1 Hz LED heartbeat. `0` saves ~17 KB. |
| `DEBUG_BAUD` | `115200` | Console speed. **Must match `monitor_speed`** in `platformio.ini`. |
| `DEBUG_LED_PIN` | `2` | On-board LED used for the heartbeat. |
| `STARTUP_SPLASH` | `1` | Panel name cards at boot. `0` boots straight into the eyes. |
| `SPLASH_SECONDS` | `5` | How long the splash counts down. |
| `BOOT_BUTTON_PIN` | `0` | Button that toggles eye artwork. |
| `SSD1327_SPI_HZ` | `8000000` | Grayscale bus speed. Lower it if long jumpers cause flicker. |
| `STARTLE_WINDUP_MS` | `1400` | Slow constrict before the startle jolt. |
| `CLOCK` | `1` | Analogue clock face. `0` compiles it out. |
| `CLOCK_*_LEN` / `CLOCK_*_HW` | — | Hand lengths and half-widths, in pixels from the iris centre. |
| `PUPIL_OFF_SCALE` | `64` | Iris scale used when the pupil is off. At or below 64 the pupil vanishes. |
| `NETWORK` | `1` | WiFi, NTP, web server and OTA. `0` compiles all of it out, saving ~535 KB. |
| `WEB_CMD_ENDPOINT` | `1` | The `/cmd` escape hatch. `0` leaves only the REST API. Needs `COMMANDS`, since it is a passthrough to the console. |
| `WIFI_HOSTNAME` | `frank` | DHCP and mDNS name. |
| `WIFI_CONNECT_MS` | `15000` | How long to wait on a known network before opening the portal. |
| `WIFI_PORTAL_S` | `180` | How long the portal stays up before carrying on offline. |
| `TZ_DEFAULT` | US Pacific | Timezone before one is saved. |
| `STARTLE_HOLD_MS` | `1200` | How long the eyes stay wide afterwards. |

Inherited from upstream, unchanged:

| Option | Default | Effect |
| :----- | :------ | :----- |
| `TRACKING` | on | Eyelids follow the pupil. |
| `AUTOBLINK` | on | Eyes blink on their own. |
| `IRIS_MIN` / `IRIS_MAX` | `150` / `400` | Pupil range. Counter-intuitively, `IRIS_MIN` is the **widest** pupil — the value divides into the iris map. |

Pin assignments (`DISPLAY_DC`, `DISPLAY_RESET`, `SELECT_L_PIN`, `SELECT_R_PIN`,
`MOSI_PIN`, `SCLK_PIN`) are wiring, not preference — change them only if you
wire differently, and update [docs/WIRING.md](docs/WIRING.md) to match.

One switch is derived rather than set: `CONTROLLABLE` is `COMMANDS || NETWORK`,
and gates the machinery both interfaces share — the operations layer in
[`src/state.h`](src/state.h), the settings it persists, and the state the
renderer reads. Turning off *both* interfaces leaves the eyes running on their
own with nothing able to change them, which builds and works but is only
useful if you want a prop with no controls at all.

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
| `save` | Remember the eye design and swap across reboots |
| `forget` | Clear saved settings |
| `net [quiet]` | Address info, on the panels too |
| `net off` | Dismiss the address cards early |
| `wifi` | The network, and how to change it |
| `wifi join <ssid> [pass]` | Store a network and reboot into it |
| `wifi forget` | Clear the stored network |
| `wifi portal` | Reboot into the setup portal |
| `tz [zone]` | Timezone by name or POSIX string |
| `splash` | Re-show the panel name cards |
| `status` | Current eye, gaze, dilation, heap, uptime, frame rate |
| `help` | The list above |

The **BOOT button** toggles the eye artwork, which is handy on the bench but
unreachable once the head is assembled — hence the console.

Overrides are sticky: `look` and `dilate` hold their commanded value until you
return them with `auto`. The autonomous animation keeps running underneath, so
handing control back is seamless.

## Choosing which eyes are built in

**[See the gallery: every design rendered in colour and greyscale &rarr;](docs/EYES.md)**

25 designs ship with the project: two from Adafruit's original Uncanny Eyes,
and 23 converted from [TeensyEyes](https://github.com/chrismiller/TeensyEyes).
Each costs roughly **158 KB of flash**, so about four fit alongside everything
else on the default partition — they are chosen at build time rather than all
compiled in.

Edit `include/eyes_config.h`:

```c
#define EYE_DEFAULT 1   // Standard human-ish hazel eye
#define EYE_NEWT    1   // Eye of newt
#define EYE_DRAGON  0   // Fiery dragon, slit pupil
...
```

Or override without touching the file, from `platformio.ini`:

```ini
build_flags = -DEYE_DEFAULT=0 -DEYE_NEWT=0 -DEYE_DRAGON=1 -DEYE_SKULL=1
```

Every switch is `#ifndef`-guarded, so a `-D` always wins. Enabling none fails
with a clear `#error` — the eye headers are also where the `SCLERA_*`,
`IRIS_*` and `SCREEN_*` dimensions come from.

The console lists whatever ended up in the build, and selects by name or
number:

```
> eye
  0  default     <- current
  1  dragon
  2  skull
> eye dragon
ok eye=1 dragon
```

### On greyscale panels

Designs that carry their character in *hue* rather than *brightness* flatten
out badly once converted to 16 grey levels. The gallery shows both side by
side — compare before committing. Designs with strong tonal structure, like
`skull`, `demon` and `spikes`, survive the conversion best.

### Adding or regenerating designs

The converted headers are generated, and the generator is checked in:

```sh
git clone --depth 1 https://github.com/chrismiller/TeensyEyes.git
pip install pillow
python tools/gen_eyes.py TeensyEyes/resources/eyes/240x240
```

That writes `include/eyes/*.h` and the gallery images. Then add an `EYE_FOO`
switch to `include/eyes_config.h` and a registry row to `src/main.cpp`.

Dimensions must match what is already built in: **SCLERA 200×200, IRIS_MAP
256×64, SCREEN 128×128, IRIS 80×80**. The renderer reaches the artwork through
pointers whose row width is fixed at compile time, so designs of different
sizes cannot coexist in one build.

Budget roughly four designs on the default 1.25 MB app partition;
`board_build.partitions = min_spiffs.csv` buys 1.9 MB while keeping OTA.

## If the panels are wired the wrong way round

`swap` exchanges the chip-select pins in software, so the panel on `D15`
becomes the one on `D4` and vice versa:

```
> swap
ok swap=on
```

This is a real swap, not a relabelling — everything belonging to an eye moves
with it, including its mirrored eyelids and its splash label. Verify with
`splash`, or reboot and read the name cards.

The swap is applied between frames, never mid-transaction, so it cannot leave
a chip select asserted on the wrong panel.

## Remembering settings

Settings can be stored in NVS, so a sealed head comes back the way you left
it:

```
> eye dragon
> swap on
> pupil off
> save
ok saved eye=dragon swap=on
```

| Saved | Not saved |
| :---- | :-------- |
| Eye design | The clock's time |
| Panel swap | Gaze (`look`) |
| Pupil on/off | Dilation (`dilate`) |
| Clock on/off, rate, second hand, hand colours | |
| Timezone | |

Wi-Fi credentials are the exception to all of this: they are stored by the
radio in its own part of NVS, not by `save`, and `forget` does not clear
them. `wifi forget` does. The control page says the same thing on each card,
so you never have to come back here to find out what a control will do.

Gaze and dilation are deliberately transient — they are things you drive,
not things you configure.

The clock's **time** is excluded on purpose. A time saved at power-off comes
back wrong by exactly the interval the device was off, and the plan is to
take the time from NTP, which makes a stored one pointless as well as
misleading. Its display preferences are saved; only the time is not, so
`clock set` is the one clock command that does not mark settings unsaved.

`status` marks unsaved changes with `(unsaved)`. `forget` clears the stored
settings and the build defaults apply again at the next boot.

Saving is explicit rather than automatic: NVS writes have finite endurance,
and the BOOT button cycles eye designs, so auto-saving would write flash on
every press.

The design is stored **by name**, not by index. Indices shift whenever the set
of `EYE_*` switches changes, so a saved index could silently select a
different design after a rebuild. If a saved design is not in the current
build, the console says so at boot and falls back to the first one.

## Pupil

`pupil off` removes the pupil entirely, leaving a full iris disc:

```
> pupil off
ok pupil=off (full iris disc; dilate has no effect)
```

The iris is drawn where `iScale * distance / 128 < 64`, and distance peaks at
127 at the centre, so any scale at or below 64 keeps every pixel in the iris.
There is nothing left to dilate, which is why `dilate` stops having an effect.

Useful on its own, and it pairs with the clock — a full disc makes a better
dial than a ring around a pupil.

## Clock face

**Experimental.** Turns the iris into an analogue clock with hour, minute and
optional second hands.

```
> clock on
> clock set 10:10
> clock rate 600           # 10 minutes of clock per second
> clock color sec FF8800   # a pop colour on the second hand
```

There is no real time source yet, so the clock free-runs from `millis()` off a
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

Set `CLOCK` to 0 in `src/main.cpp` to compile the whole feature out.

## Startup splash

At boot, each panel names itself for five seconds:

```
   FRANK'S
    RIGHT
  ─────────
    YOUR
    LEFT
      5
```

Both perspectives are shown because "left eye" is ambiguous in every wiring
table ever written. This is the quickest way to confirm which panel is on
which chip select without getting at the wires.

## Network

The board joins WiFi at boot, answers to **`frank.local`**, serves a control
page and a REST API, and accepts firmware over the air.

### Credentials

Copy the template and fill it in — it is gitignored, so nothing secret is ever
committed:

```sh
cp include/secrets.h.example include/secrets.h
```

```c
#define WIFI_SSID "YourNetwork"
#define WIFI_PASS "YourPassword"
```

It is optional. A build without it still compiles, and an unconfigured board
opens a **setup portal** instead: join the `frank-setup` network from a phone
and pick your WiFi. The panels display the network name while the portal is
up, so a head sitting there is not a mystery.

Three sources are tried in order of how deliberate they are: whatever the
portal last stored, then the build-time defaults, then the portal. Stored
credentials win because they were an explicit choice made on that device.

Nothing here is fatal — a board that cannot reach a network carries on being a
pair of eyes.

> **Why a header rather than `secrets.ini` and `build_flags`**, which would be
> the more idiomatic PlatformIO route: `build_flags` are processed by SCons,
> which uses `$` as its own substitution character. A password containing `$`
> is silently truncated there — no error, just a shorter string and a board
> that will not associate. Doubling the `$` does not help. The preprocessor
> reads a header directly, so only ordinary C string escaping applies.

### Changing networks later

The portal is not the only way in once the head is sealed. `wifi join`, over
serial or from the control page, stores a network and reboots into it:

```
> wifi join spare-network hunter2
ok storing 'spare-network'; rebooting
```

`wifi portal` reboots into the setup portal on demand, and `wifi forget`
clears the stored network so the build-time credentials apply again.

Each of these reboots rather than reconnecting in place. Reconnecting would
mean re-running mDNS, SNTP, the web server and OTA and getting every one of
them idempotent; rebooting reuses the path that already works, and the board
is back in about eight seconds.

The three sources of credentials are genuinely distinct: the build-time
defaults are applied with the radio's storage set to RAM, so connecting with
them does not quietly turn them into a stored network — otherwise `wifi
forget` would look like it had not worked the moment the board reconnected.

### Time

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

Defaults to US Pacific, and is persisted.

Once time is synced, `clock set` and `clock rate` stop having any effect:
they drive the free-running fallback, which is no longer what feeds the hands.

### Web interface

**http://frank.local/** — a control page for everything the console can do:
eye design, gaze, dilation, pupil, panel swap, clock and hand colours,
timezone, Wi-Fi, and the address details. It polls the device once a second,
so two browsers looking at it stay in step with each other and with anything
you type over serial.

Each card says what happens to its settings when the power goes off — saved,
session only, or momentary — because that is the first question anyone asks
of a control they have just moved.

The page is static: one 17 KB string in [`src/page.h`](src/page.h), served
straight out of flash. Everything on it is drawn from the API below, so there
is no markup anywhere that has to be kept in step with device state.

Requests are served from the render loop, so each one costs a dropped frame
or two. That is why the page polls at a leisurely rate and the responses are
kept small.

### REST API

Everything the page does, `curl` can do. **`/api/v1`**, JSON in and JSON out,
CORS open so a page served from anywhere can drive the device.

| Method | Path | What it does |
| :----- | :--- | :----------- |
| `GET` | `/api/v1/state` | Everything at once — what the page polls |
| `GET` | `/api/v1/eyes` | The eye designs this firmware was built with |
| `GET` | `/api/v1/net` | MAC, addresses, signal, sync state |
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
build does not contain, `405` for the wrong verb on a real path.

Every write to `/wifi` answers first and then reboots the board, so the reply
arrives but the connection it arrived over does not survive. `GET /wifi`
never returns a password.

Gaze runs `0`–`1023` on each axis with **`y=1023` at the top**, the way a
joystick reads rather than the way a screen does. `512 512` is centre. The
control page flips it so that dragging up looks up.

There is no authentication. Anything that can reach the board can drive it,
which is the right trade for a prop on a home network and the wrong one for
anywhere else.

### The `/cmd` escape hatch

For anything the API does not model yet, `/cmd?c=<command>` hands a line
straight to the console's dispatcher:

```sh
curl "http://frank.local/cmd?c=help"
curl "http://frank.local/cmd?c=status"
```

It returns plain text, not JSON, and it is a convenience rather than an
interface — prefer the API for anything you are writing against. Set
`WEB_CMD_ENDPOINT` to `0` to leave it out.

### IPv6

The board brings up a link-local IPv6 address and answers pings on it, but
**the web server is IPv4 only** — `WiFiServer` in the ESP32 Arduino core
opens an `AF_INET` socket and nothing else, so there is nothing listening on
the v6 address. `GET /api/v1/net` reports this as `"ipv6Served": false`
rather than leaving you to work it out from an address that does not answer.

Two things would have to change to make `http://[…]/` work: the core would
have to move to 3.x, where the server is dual-stack, and the board would need
a global address rather than a link-local one, which needs the router to
advertise a prefix. Until then, use the IPv4 address or `frank.local`.

### Address info on the panels

`net` reports over serial and paints both panels for twelve seconds — Frank's
right shows MAC, IPv4 and signal, his left shows IPv6 and the mDNS name. A
link-local IPv6 address is 39 characters and a panel holds 21, so it is
wrapped rather than truncated, and split across the two displays.

Twelve seconds is a long time to stare at a MAC address, so the cards can be
dismissed: `net off` over serial, the same button on the control page, or
`PUT /api/v1/netinfo {"on":false}`. `net quiet` reports without touching the
panels at all.

### Over-the-air updates

```sh
pio run -e gray_ota -t upload
```

Progress shows on the panels. The eyes stop during the transfer — that is
expected, not a hang.

Two things bite on Windows:

- **`frank.local` will not resolve** unless Bonjour is installed; Windows has
  no mDNS resolver of its own. The device advertises correctly. Use the
  address instead: `--upload-port 192.168.1.50`.
- **"No response from device"** means espota advertised the wrong local
  interface for the board to call back to, which happens when VMware, WSL or
  VirtualBox have each added one. Pin it with
  `upload_flags = --host_ip=192.168.1.20`.

macOS and Linux need neither workaround.

## Troubleshooting

| Symptom | Cause |
| :------ | :---- |
| **Both panels show white static** | Wrong controller. You have SSD1327 panels and built `esp32dev`, or vice versa. Confirm with `pio run -e probe1327 -t upload -t monitor` — if the panels respond to that, they are SSD1327; build `gray`. |
| Both panels dark | Power, or `D27` (RST) not connected. Check 3V3 and GND at the module pins. |
| One panel dark | That panel's `CS` wire. Everything else is shared, so a shared fault would take out both. |
| Flicker or speckle | Bus integrity. Shorten the `CLK` and `DIN` jumpers, or lower `SSD1327_SPI_HZ`. |
| Garbled serial output | `DEBUG_BAUD` and `monitor_speed` disagree, or a CH340 clone struggling above 115200. |
| Board resets when a panel is connected | Brownout. Use a powered hub or feed 5 V to `VIN`. |
| Board will not boot | Something on `D12`. Held high at reset it stops the ESP32 starting. No eye signal uses it. |

Frame rate is **not** a useful signal for whether panels are connected — the
SPI writes happen either way, so the rate is the same with nothing attached.

For bring-up, `displaytest` drives each panel alone with solid fills at a
deliberately slow 2 MHz, which separates wiring faults from rendering faults.

## Credits

Lineage, oldest first:

- **[Adafruit Uncanny Eyes](https://learn.adafruit.com/animated-electronic-eyes)** — Phil Burgess / Paint Your Dragon, for Adafruit Industries. The rendering engine and the eye artwork. SPI FIFO insight from Paul Stoffregen's `ILI9341_t3`; concept inspired by David Boccabella (Marcwolf).
- **Laurent Moll**, 2018 — [Uncanny Eyes costume](https://www.hackster.io/projects/376a13/), dual-display ESP32 work.
- **[bitcldr/creeper-eyes](https://github.com/bitcldr/creeper-eyes)** — the PlatformIO project this forks from.
- **[TeensyEyes](https://github.com/chrismiller/TeensyEyes)** — Chris Miller. MIT. 23 of the 25 eye designs are converted from its artwork by [`tools/gen_eyes.py`](tools/gen_eyes.py).
- This fork — generic ESP32 support, grayscale panels, console, splash, diagnostics.

Three bugs fixed here also affect the upstream colour build: an out-of-range
eyelid index that put a teardrop artifact at one eye's edge, a shared reset
line that wiped the first panel's initialisation while the second started, and
a chip-select constant that selected the wrong panel during init.

MIT licensed — see [LICENSE](LICENSE).
