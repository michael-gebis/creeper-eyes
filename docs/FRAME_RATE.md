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
| CPU at 160 MHz instead of 240 | 51 | 5.8 ms | 19.4 ms | 1.1 ms |

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

## Brownouts, and the CPU at 160 MHz

Soaking is the API test suite run against the board over and over, with the
serial port watched throughout. The runs include eye-file uploads, which write
flash and pause both cores. The first short soaks of the overlap passed, apart
from one software reset that nothing explained. So the board was taught to
report why it last restarted (below), and the soaks were made longer. Same
board, same single USB-C supply, C1 not fitted, 45 minutes each:

| build | suite runs | brownouts |
| :-- | --: | --: |
| No overlap, CPU 240 MHz (41 fps) | 26 | 0 |
| Overlap, CPU 240 MHz (53 fps) | 26 | 4 |
| Overlap, CPU 160 MHz (51 fps) | 25 | 0 |

Every brownout came about a second after the render loop went back to full
speed after a pause: three after an eye-file upload finished, one after the
address cards. That is the step from mostly idle to both cores flat out, drawing
and sending, while WiFi answers the request. The ESP32's brownout detector is
already at its most lenient setting, about 2.4 V, so the 3.3 V rail really was
sagging that far.

With the overlap the frame waits on the send, not the CPU, so 160 MHz costs two
frames a second. It also cuts the current by enough that the same 45 minutes
passed clean. With four brownouts expected at the old rate, a clean run by
chance would be about a 2% likelihood. So the CPU runs at 160 MHz
(`board_build.f_cpu` in `platformio.ini`).

That is a margin, not a cure. A board that browns out at 240 MHz is running
close to its supply's limit, and C1 — as large as the 5 mm footprint takes,
typically 22 or 47 µF, rather than 10 — or a stronger supply is the real fix.

The board now reports any restart caused by a crash, a watchdog or a brownout
as a [warning](CONFIG.md#if-stored-settings-are-damaged), on the control page
and the console. The brownouts above were how it was confirmed: each one showed
up there.

## What is left

The send is now the whole frame, about 19 ms at 16 MHz. Only a faster bus (20 MHz
speckled one panel here) or fewer bytes per frame would go further. Driving the
bus by DMA instead of the FIFO would free core 0 from busy-waiting, but would not
make frames faster.

## Greyscale

Frank, with greyscale panels, measured with `main` flashed over USB — the
flash settings and the RAM tables, but not the overlap:

| build | fps | draw | send | other |
| :-- | --: | --: | --: | --: |
| As it was: flash at 40 MHz DIO | 33 | — | — | — |
| Flash at 80 MHz QIO, `RAM_TABLES` | 55 | 8.3 ms | 8.5 ms | 1.0 ms |

A greyscale frame is 8 KB, sent at 8 MHz in about 8.5 ms, so here the drawing
and the sending take about as long as each other. Drawing takes longer than on
colour, 8.3 ms against 4.5; greyscale's draw time includes packing the finished
frame down to 4-bit grey, two pixels a byte. With the two about equal, the
overlap should help greyscale most: hiding the send would leave a frame of about
9.5 ms at 240 MHz. At 160 MHz the drawing slows by about half, to roughly 12 ms,
so it is the drawing rather than the send that would set the pace. The overlap
on greyscale has not been measured.
