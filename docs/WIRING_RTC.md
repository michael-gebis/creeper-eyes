# Wiring the battery-backed clock — optional

**You do not need this.** The eyes, the clock face, and everything else work
without it. Skip this page unless you want the head to know what time it is
after a power cut, or without a network at all.

Four wires, one small board, and a coin cell.

## What it buys you

| | Without an RTC | With one |
| :--- | :--- | :--- |
| No network, ever | Time resets to 10:10 at every boot; set it by hand each time | Set it **once**; kept for the life of the cell |
| Network, then a power cut | Right again a few seconds after NTP answers | Right immediately at boot |
| Network goes away for good | Free-runs and drifts | Keeps time to about a minute a year |

The DS3231 is temperature-compensated — ±2 ppm, so roughly **a minute a
year**. That is far better than the ESP32's own crystal, which is what the
free-running fallback rides on.

## What to buy

Any **DS3231** module. These are sold as "DS3231 AT24C32 IIC precision RTC
module" or by the board name **ZS-042**, usually in multipacks of three or
four for a few pounds. The one this was written against is
[a four-pack of the 3.3V/5V board](https://www.amazon.com/dp/B08X4H3NBR).

They carry an AT24C32 EEPROM alongside the clock chip. This project does not
use it; it sits harmlessly at a different address.

Two things the listings do not tell you:

- **The coin cell is usually not included** — lithium cells are awkward to
  ship. You want a **CR2032**, or a rechargeable **LIR2032**; see the warning
  below before you choose.
- **Do not buy a DS1307.** It looks identical and is a fraction of the price,
  but it is 5V-only and has no temperature compensation, so it drifts by
  minutes a month.

## Connections

Four wires. The board is `3V3`-tolerant, which is what we want anyway — see
the battery warning.

| Signal | DevKit pin | RTC module | Purpose |
| :----- | :--------- | :--------- | :------ |
| VCC | `3V3` | VCC | Power. **Not VIN** — see below |
| GND | `GND` | GND | Common ground |
| SDA | `D21` | SDA | I²C data |
| SCL | `D22` | SCL | I²C clock |
| — | — | `32K` | Leave empty |
| — | — | `SQW` | Leave empty |

GPIO21 and GPIO22 are the ESP32's default I²C pins and are **not used by the
displays**, so this adds nothing to the existing harness — it hangs off two
otherwise idle pins and the power rails you already ran.

```
  ┌────────────┐
  │   ESP32    │        ┌───────────────────┐
  │            │        │  DS3231 / ZS-042  │
  │  3V3   ────┼────────┤ VCC               │
  │  GND   ────┼────────┤ GND               │
  │  D21   ────┼────────┤ SDA        (coin  │
  │  D22   ────┼────────┤ SCL         cell) │
  │            │        │ 32K  ── unused    │
  │            │        │ SQW  ── unused    │
  └────────────┘        └───────────────────┘

  The displays keep D18, D5, D33, D27, D15 and D4 exactly as they were.
```

Suggested jumper colours, to stay consistent with
[the eye harness](WIRING.md): red VCC, black GND, and two colours you have not
already used for SDA and SCL.

## Read this before you fit a battery

The ZS-042 board has a trickle-charging circuit — a 200 Ω resistor and a
1N4148 diode — designed for a **rechargeable LIR2032**. Boards are very often
sold or fitted with a **non-rechargeable CR2032** instead, and continuously
trickle-charging one of those is a documented way to make it leak or swell.

**Powering the module from 3V3 avoids the problem entirely.** The diode never
forward-biases at that voltage, so no charging current flows, whichever cell
is fitted. The wiring above is therefore already the safe configuration, and
it is one more reason not to reach for `VIN`.

If you have some reason to run it at 5V, then either fit a LIR2032 as the
board intends, or remove the charging resistor.

Sources: [One Transistor](https://www.onetransistor.eu/2019/07/zs042-ds3231-battery-charging-circuit.html),
[Arduino Forum](https://forum.arduino.cc/t/mods-to-ds3231-zs-042-module-for-power-control/1101164)

## Silkscreen and pin order vary

As with the panels, the boards are not all laid out the same way. Some have a
six-pin header on each long edge, mirrored, so the module can be dropped onto
a breadboard either way up. **Read the labels, not the positions** — SDA and
SCL swapped is the single most likely reason for a module that will not
answer, and it does no harm to the chip.

## Building it in

The RTC is compiled out by default. Turn it on with one flag:

```sh
pio run -e gray_rtc -t upload          # grayscale panels + RTC
pio run -e esp32dev_rtc -t upload      # colour panels + RTC
```

Those two environments are just `gray` and `esp32dev` with `-DRTC=1`; you can
add the same flag to any environment of your own. It costs about **28 KB** of
flash, which is the I²C library plus the driver.

Different pins, if you need them:

```ini
build_flags = -DRTC=1 -DRTC_SDA_PIN=16 -DRTC_SCL_PIN=17
```

A build with `RTC=1` and no module attached is **not an error**. The probe at
boot finds nothing, says so on the console, and the clock free-runs exactly as
it did before. You can wire the module up later without reflashing.

## First power-on

```
> rtc
rtc addr=0x68 sda=21 scl=22 present
  battery held; the time is good
  chip    2026-09-10 16:34:12 UTC
  temp    24.75 C
```

A brand-new module has never been set, so expect this instead:

```
  battery lost or never set; the time is not to be believed
```

Set it, and it is kept from then on:

```
> tz chicago
ok tz=CST6CDT,M3.2.0/2,M11.1.0/2
> clock set 16:34
ok clock=16:34:00
> rtc
  battery held; the time is good
```

With a network, you need not do any of that — the first NTP sync writes the
chip by itself, and says so:

```
[net] time synced: 2026-09-10 16:34:12 CST6CDT,M3.2.0/2,M11.1.0/2
[rtc] written from ntp
```

## If it does not answer

Take the firmware out of the picture:

```sh
pio run -e rtc_probe -t upload -t monitor
```

That scans the whole I²C bus and reads the chip raw, several times a second:

```
scan: found 0x57  (AT24C32 EEPROM)
scan: found 0x68  (DS3231)
status 0x00  oscillator has run continuously
raw 00..06: 12 34 09 03 10 09 26
-> 2026-09-10 09:34:12 UTC   temp 24.75 C
```

| What you see | What it means |
| :--- | :--- |
| `nothing on the bus` | No power, no ground, or SDA and SCL swapped |
| Only `0x57` | The EEPROM is alive but the clock half is not — a faulty board |
| Only `0x68` | Fine. Some boards omit the EEPROM |
| `OSCILLATOR HAS STOPPED` | Never set, or the cell is dead or missing |
| Right date, wrong hour | Expected — the chip holds **UTC**. Set `tz` |

## Why the chip holds UTC

Because DST would otherwise be wrong for half the year on a board with no
network. Storing local time would mean a head unplugged in February and
switched on in July reads an hour out, with nothing available to correct it.
Storing UTC and applying the timezone on the way out gets it right offline,
which is the whole point of fitting a battery.

This is why `tz` works in no-network builds now. It is a setting like any
other, saved with `save`.

## Time source precedence

More than one thing can know the time. They are ranked, and a better source
is never overridden by a worse one:

```
  free-running   ← boots at 10:10, drifts
       ↓
  the RTC        ← read once at boot
       ↓
  set by hand    ← `clock set`; you are correcting it, so you win
       ↓
  NTP            ← a time server answered; also writes the RTC
```

`clock` and the control page both report which one is in charge, and so does
`GET /api/v1/state` as `clock.source`.
