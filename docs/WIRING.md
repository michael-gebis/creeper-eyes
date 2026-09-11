# Wiring the eyes

Two 1.5" 128×128 OLED panels onto an ESP32 DevKit V1. Eight signals, fourteen
wires, one shared SPI bus.

This is the same for **both** panel types — the Waveshare 1.5inch RGB OLED
(SSD1351) and the 1.5inch OLED (SSD1327) have identical 7-pin headers. Only
the build differs: `esp32dev` for colour, `gray` for grayscale.

## Connections

Six of the eight signals are shared by both panels. Only chip select differs —
that is the whole trick to driving two panels off one bus.

Left and right are **Frank's own**, the way anatomy is always described: facing
him, his right eye is the one on your left.

| Signal | DevKit pin | Frank's right (your left) | Frank's left (your right) | Purpose |
| :----- | :--------- | :------------------------ | :------------------------ | :------ |
| VCC | `3V3` | VCC | VCC | Power. **Not VIN.** |
| GND | `GND` | GND | GND | Common ground, both panels |
| DIN | `D18` | DIN | DIN | SPI data — pixels travel here |
| CLK | `D5` | CLK | CLK | SPI clock |
| DC | `D33` | DC | DC | Data / command select |
| RST | `D27` | RST | RST | Shared reset, pulsed once at boot |
| CS | `D15` | CS | — | Selects Frank's right panel only |
| CS | `D4` | — | CS | Selects Frank's left panel only |

There is **no MISO connection**. The panels never talk back, so GPIO19 — which
the code reserves for it — stays empty and is free for anything else.

Suggested jumper colours, so the harness stays readable once it is buried in a
head: red VCC, black GND, yellow DIN, blue CLK, green DC, violet RST, and two
distinct colours for the chip selects.

## Bus topology

```
                     ┌──────────────┐
                     │  Frank's     │
              ┌─────►│  RIGHT eye   │
              │   ┌─►│  (your left) │
              │   │  └──────────────┘
  ┌────────┐  │   │
  │        │  │   │   VCC GND DIN CLK DC RST   shared by both
  │ ESP32  ├──┴───┼── ────────────────────────────────────┐
  │        │      │                                       │
  │  D15   ├──────┘   CS, Frank's right only              │
  │  D4    ├──────┐   CS, Frank's left only               │
  └────────┘      │  ┌──────────────┐                     │
                  └─►│  Frank's     │◄────────────────────┘
                     │  LEFT eye    │
                     │ (your right) │
                     └──────────────┘
```

Six signals fan out to both panels, two chip selects go one each: 6×2 + 2 = 14
physical connections.

## Before you connect anything

### 3V3, never VIN

The ESP32's GPIOs are 3.3 V and **not 5 V tolerant**. `VIN` carries 5 V
straight from USB.

The Waveshare modules are 3.3 V / 5 V tolerant and will survive either, but
their logic pins would then be driving 5 V back into the ESP32. Use `3V3`. It
is the correct rail for both panel types, and no level shifting is needed.

> Waveshare's own documentation warns: *"Please ensure that the power supply
> voltage and logic voltage are consistent, otherwise it will not work
> properly."* Powering from 3V3 keeps both at 3.3 V.

### Silkscreen labels vary

Modules from different sellers name the same pins differently:

| This guide | Also seen as |
| :--------- | :----------- |
| **DIN** | SDA, SI, MOSI, DATA |
| **CLK** | SCL, SCK, D0 |
| **RST** | RES, RESET |
| **DC** | D/C, A0, RS |
| **CS** | OCS, SS |

A module labelled SDA/SCL is still **SPI** here, not I²C. Match by position in
the table, not by the letters.

### Interface mode

Both Waveshare modules ship configured for **4-wire SPI**, which is what this
project uses. That is set by solder jumpers on the back — `BS0` on the RGB
module, `BS1`/`BS2` on the grayscale one. If you have not touched them, they
are already correct.

### Power budget

Two 128×128 OLEDs draw roughly 20–40 mA each at typical content, more on bright
frames. With the ESP32 idle at 40–80 mA that fits a 500 mA USB port
comfortably.

It stops being comfortable if you add Wi-Fi later — transmit bursts spike hard.
Plan a proper supply then, not now.

### Use the breadboard rails

Run one wire from `3V3` to the red rail and one from `GND` to the blue, then
tap both panels off the rails. Fourteen connections become ten jumpers from the
board, and the power runs stay short.

## Order of assembly

Power off and USB unplugged throughout.

1. **Ground first.** `GND` to the blue rail, then a lead to each panel. A panel
   powered without a common ground can find its return path through a data pin.
2. **Then 3V3.** To the red rail, then to each panel. Confirm you are on `3V3`
   and not the adjacent `GND` or `VIN` — on the 30-pin board `3V3` is the very
   last pin on the right-hand header.
3. **Shared signals.** `D18`→DIN, `D5`→CLK, `D33`→DC, `D27`→RST, each landing
   on both panels.
4. **Chip selects last.** `D15` to Frank's right panel, `D4` to his left. These
   are the only two wires that differ, so they are the two worth
   double-checking.
5. **Re-check 3V3 and GND** at both modules before plugging in USB. Reversed
   power is the one mistake that ends the evening.

## First power-on

Flash the build that matches your panels — `esp32dev` for SSD1351 colour,
`gray` for SSD1327 grayscale — then power up.

### What correct looks like

1. Each panel names itself for five seconds: `FRANK'S RIGHT` over `YOUR LEFT`,
   or the reverse.
2. Both panels animate: irises drifting and rescaling, occasional blinks.
3. The blue on-board LED blinks at 1 Hz.

**If the labels are swapped** relative to the face, the panels are on the
opposite chip selects from what the code assumes. That is a two-line fix in
`showSplash()`, far easier than rewiring.

### The frame rate will not change

The serial heartbeat reports around `fps=20` on colour panels and `fps=40` on
grayscale — and it reports the same with **nothing connected at all**. The SPI
peripheral clocks the same bytes out whether a panel is listening or not.

So an unchanged frame rate tells you *nothing* about whether the panels are
wired correctly. Trust your eyes, not the number.

### Watch the console

```sh
pio device monitor
```

A `rst:0xc` or a brownout message the instant you connect the second panel
points at power, not at data wiring.

## If something is off

| Symptom | Most likely cause |
| :------ | :---------------- |
| **Both panels show white static** | Wrong controller for the build. SSD1351 init means nothing to an SSD1327, and vice versa — the panel shows uninitialised memory. Colour panels show *coloured* speckle; monochrome ones show white. Run `pio run -e probe1327 -t upload -t monitor`: if the panels respond, they are SSD1327 and you want the `gray` build. |
| Both panels dark | Power, or `D27` held low. Check 3V3 and GND at the module pins, not at the rail. A panel held in reset stays black. |
| One panel dark, one fine | That panel's `CS`, or its own power. Everything else is shared, so a shared-signal fault would take out both. |
| Noise or garbage, not static | `DC` on `D33`, or clock integrity. Commands are being read as pixels, or the reverse. |
| Flicker, tearing, speckle | Breadboard signal integrity. Shorten the `CLK` and `DIN` jumpers, keep them away from the power runs, and lower `SSD1327_SPI_HZ` if you are on grayscale. |
| Both panels show the same content | A `CS` line shorted or on the wrong row — both panels selected at once. |
| Board resets when a panel connects | Brownout. Move to a powered hub or feed 5 V to `VIN`. |
| Board will not boot at all | Check nothing has crept onto `D12`. Held high at reset it stops the board starting. None of the eye signals use it. |

Isolating a fault is easiest one panel at a time: unplug one entirely and
confirm the other works alone, then swap. The chip selects are independent, so
either panel runs fine on its own.

For a lower-level check, `displaytest` drives each panel with nothing but solid
colour fills at 2 MHz:

```sh
pio run -e displaytest -t upload -t monitor
```

If solid colours appear there, the wiring, the bus and the init sequence are
all good, and any remaining fault is in the eye rendering.

## Pin reference

Every pin this project uses is broken out on the 30-pin DevKit V1:

| GPIO | Silkscreen | Role here | Notes |
| :--- | :--------- | :-------- | :---- |
| 4 | `D4` | CS, Frank's left | |
| 5 | `D5` | CLK | Strapping pin; must be high at boot, which SPI idle satisfies |
| 15 | `D15` | CS, Frank's right | Strapping pin; CS idles high, so this is fine |
| 18 | `D18` | DIN | |
| 19 | `D19` | *(reserved MISO)* | Unused — free |
| 21 | `D21` | *(free)* | I²C data, if the optional RTC is fitted |
| 22 | `D22` | *(free)* | I²C clock, if the optional RTC is fitted |
| 27 | `D27` | RST | |
| 33 | `D33` | DC | |
| 2 | *(no header pin)* | Heartbeat LED | On-board blue LED |
| 0 | *(no header pin)* | Eye toggle | BOOT button |

`GPIO12` is deliberately unused: held high at reset it selects a 1.8 V flash
voltage and the board will not start. Keep it clear.

## Optional add-ons

None of these are needed for the eyes to work, and each is compiled out by
default:

- **[A battery-backed clock](WIRING_RTC.md)** — a DS3231 on four wires, so the
  head knows what time it is after a power cut or with no network at all.
