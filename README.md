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
- **Startup splash** naming each panel, so you never have to trace wires
- **BOOT button** toggles between the two eye designs
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

Compile-time switches, all in `src/main.cpp` unless noted.

| Option | Default | Effect |
| :----- | :------ | :----- |
| `USE_SSD1327` | `0` | Selects the grayscale panel driver. Set by the `gray` environment via `build_flags`, not edited by hand. |
| `COMMANDS` | `1` | Serial console and BOOT-button toggle. `0` compiles both out. |
| `DEBUG` | `1` | Serial diagnostics and the 1 Hz LED heartbeat. `0` saves ~17 KB. |
| `DEBUG_BAUD` | `115200` | Console speed. **Must match `monitor_speed`** in `platformio.ini`. |
| `DEBUG_LED_PIN` | `2` | On-board LED used for the heartbeat. |
| `STARTUP_SPLASH` | `1` | Panel name cards at boot. `0` boots straight into the eyes. |
| `SPLASH_SECONDS` | `5` | How long the splash counts down. |
| `BOOT_BUTTON_PIN` | `0` | Button that toggles eye artwork. |
| `SSD1327_SPI_HZ` | `8000000` | Grayscale bus speed. Lower it if long jumpers cause flicker. |
| `STARTLE_WINDUP_MS` | `1400` | Slow constrict before the startle jolt. |
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

## Serial console

Open `pio device monitor` and type `help`. Commands are line-based at 115200.

| Command | Effect |
| :------ | :----- |
| `eye default \| newt \| toggle` | Swap the eye artwork |
| `look <x> <y>` | Aim the gaze; each 0–1023, `512 512` is centre |
| `look auto` | Hand gaze back to autonomous motion |
| `dilate <0-100>` | Pupil width; `100` is fully dilated |
| `dilate auto` | Hand dilation back to autonomous |
| `startle` | Constrict slowly, then snap wide with a blink |
| `blink` | Blink both eyes now |
| `splash` | Re-show the panel name cards |
| `status` | Current eye, gaze, dilation, heap, uptime, frame rate |
| `help` | The list above |

The **BOOT button** toggles the eye artwork, which is handy on the bench but
unreachable once the head is assembled — hence the console.

Overrides are sticky: `look` and `dilate` hold their commanded value until you
return them with `auto`. The autonomous animation keeps running underneath, so
handing control back is seamless.

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
- This fork — generic ESP32 support, grayscale panels, console, splash, diagnostics.

Three bugs fixed here also affect the upstream colour build: an out-of-range
eyelid index that put a teardrop artifact at one eye's edge, a shared reset
line that wiped the first panel's initialisation while the second started, and
a chip-select constant that selected the wrong panel during init.

MIT licensed — see [LICENSE](LICENSE).
