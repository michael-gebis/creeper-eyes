# Frame rate

How the colour eyes went from 19 frames a second to 53, measured one change
at a time on frank-dev: a DevKit on the rev A carrier board, two SSD1351 colour
panels, WiFi up, the default eye.

## Reading the numbers

The serial heartbeat prints, once a second:

```
[creeper-eyes] fps=53 draw=4.2ms wait=13.1ms send=18.4ms other=1.0ms heap=...
```

- **fps** counts eyes drawn, and the two eyes take turns, so each eye is
  updated half that often. At 53, each eye gets about 26 new frames a second.
- **draw** is computing one eye's pixels.
- **send** is putting them on the panel's SPI bus.
- **wait** is the render loop waiting for the previous frame to finish
  sending. It is only non-zero with `OVERLAP_SEND`, below.
- **other** is everything else the render loop does: the web server, OTA, the
  motion code.

A frame is `draw + wait + other`. Without the overlap it is `draw + send +
other`.

## What each change did

| change | fps | draw | send | other |
| :-- | --: | --: | --: | --: |
| As it was: bus at 8 MHz, flash at 40 MHz DIO | 19 | — | ~33 ms | — |
| Colour bus to 16 MHz (`SSD1351_SPI_HZ`) | 29 | 13.2 ms | 18.5 ms | 2.2 ms |
| Compiled with `-O2` instead of `-Os` | 28.5 | 13.7 ms | 18.5 ms | 2.2 ms |
| Flash at 80 MHz DIO | 35 | 8.6 ms | 18.4 ms | 1.2 ms |
| Flash at 80 MHz QIO | 38 | 6.2 ms | 18.3 ms | 1.1 ms |
| Polar map and iris in RAM (`RAM_TABLES`) | 41 | 4.5 ms | 18.3 ms | 1.0 ms |
| Sending on the other core (`OVERLAP_SEND`) | 53 | 4.2 ms | 18.4 ms | 1.0 ms |

Medians over about twelve one-second heartbeats each. The first row predates
the timing, so only the send is known: 32 KB at 8 MHz.

### The bus

A colour frame is 128 × 128 × 2 bytes, 32 KB, four times a greyscale one, and
the colour panels ran at the Adafruit library's default of 8 MHz. The SSD1351 is
rated to 20 MHz. That gave 30–31 fps, but speckled one of the two panels on the
carrier board. 16 MHz, the next speed the ESP32's 80 MHz divides to exactly, is
clean. It is `SSD1351_SPI_HZ`.

### Drawing was waiting on flash

`-O2` changed nothing, so drawing was not short of arithmetic. It was reading
the eye artwork: about 128 KB of tables per frame, out of flash, through a cache
of about 32 KB. Every miss waits on the flash chip, and the board definition
runs it conservatively at 40 MHz on two data lines. At 80 MHz on four (QIO),
drawing took half as long. The flash speed belongs to the bootloader, which is
written only by a USB flash. So a board updated over the air is expected to keep
its old speed until it is flashed over USB once. That has not been confirmed on
a board yet.

Of the tables, the polar map and the iris are read out of order, so they miss
the cache most. `RAM_TABLES` copies those two, 45 KB, into RAM whenever the
design changes. The sclera is read in order, which the cache handles, and it is
too big to be worth it.

### Drawing and sending at once

Once drawing was 4 ms, sending was three quarters of a frame, and nothing about
drawing could go further. `OVERLAP_SEND` moves the send to the ESP32's other
core. The render loop runs on core 1. It hands each finished frame to a small
task on core 0 and draws the next eye into a second buffer while the first goes
out, so a frame now takes as long as the send alone.

The two cores share the bus without new locking. The Arduino core's
`beginTransaction()` takes a mutex, and every panel write is one transaction, so
a command from the render loop simply waits while a frame goes out. The mutex
cannot *order* things, though, so `displayQuiesce()` waits for the queue to drain
at the three places where order matters:

- **Before a card is drawn.** A frame queued before it would otherwise paint
  over it.
- **Before a swap,** which moves the chip select that a frame in flight is using.
- **Before a flip,** which changes state the send reads.

It took three tries to keep the task watchdog happy. Sending busy-waits on the
SPI FIFO, and the render loop always has the next frame ready, so the sender
kept core 0 busy without a break. Core 0's idle task never ran, and the task
watchdog, which listens for it, aborted the board after about 25 seconds.
Running the sender at the idle task's own priority fixed that but halved the
send rate, because the idle task then took its whole tick. The watchdog needs
the idle task only once in five seconds, so the sender now steps aside for one
tick every second: about a thousandth of its time.

It costs a second frame buffer: 32 KB on colour, 8 KB on greyscale.
`OVERLAP_SEND=0` puts everything back in the render loop.

## Stability

About 25 minutes of soaking with the API test suite running against the board
throughout, including eye-file uploads, which write flash and pause both cores.
That was 12 full suite runs, 212 checks each, with no failures and no watchdog
or crash output.

**One reset is unexplained.** Early in the first soak, the board restarted once
(`rst:0xc`, a software reset) during a test run, and never again in the time
after. No crash message was captured. The filter watching the serial port then
did not look for a brownout message, which ends in the same kind of reset, and a
single USB-C supply with C1 not fitted makes a brownout plausible. To catch the
next one, the board now reports any restart caused by a crash, a watchdog or a
brownout as a [warning](CONFIG.md#if-stored-settings-are-damaged), on the
control page and the console.

## What is left

The send is now the whole frame, 18.4 ms at 16 MHz. Only a faster bus (20 MHz
speckled one panel here) or fewer bytes per frame would go further. Driving the
bus by DMA instead of the FIFO would free core 0 from busy-waiting, but would not
make frames faster.

The greyscale panels should gain the most, and have not been measured. A
greyscale frame sends 8 KB at 8 MHz, about 9 ms, so with the flash, the RAM
tables and the overlap, a greyscale frame should be close to that. Frank will
show it when he is next powered and flashed over USB.
