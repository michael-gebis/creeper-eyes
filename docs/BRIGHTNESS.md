# Brightness

One setting, 0–100%, for how bright the eyes are. 0 is off. Changes fade
rather than jump. Nothing changes until you move it: a board with nothing
saved starts at whatever setting reproduces the brightness the panels always
had.

## Using it

The **bright** slider on the control page's Eye card. It is live while you
drag it, and saved with the other settings when you press save. Under it,
**tune brightness** has the curve, a sweep, and a trim for each panel.

```
> dim
dim=90% shown=90% gamma=2.2 trim left=+0 right=+0
> dim 40
> dim 0                     # off
> dim gamma 2.8             # the curve; 1.0 is linear
> dim trim right -15        # take the right panel down 15%; your left and right
> dim sweep                 # slowly full to nearly off and back; `dim sweep off`
```

Or `PUT /api/v1/dim` with any of `{"percent":40}`, `{"gamma":2.8}`,
`{"trim":{"left":-15}}` and `{"sweep":true}`. `GET` reports the same, plus
`shown`, which is what the panels are showing at that moment and differs from
`percent` during a fade, a sweep, or sleep.

## Which curve

Eyes do not see light linearly: half the current does not look half as
bright. The slider maps to light by `(percent / 100) ^ gamma`, so the curve
decides where the slider's resolution goes. Linear (1.0) spends most of the
slider on levels that all look bright. Steeper curves give more room to the
dim end.

**2.2 is the default**, as it is for most displays. It is a starting point, not
an answer: the panels, the eye designs and the room all move it. To compare:

1. Pick a curve on the page, or `dim gamma 2.8`. It applies immediately,
   because the setting stays put and only the light it maps to moves.
2. Press **sweep**, or `dim sweep`. The eyes run from full to nearly off over
   eight seconds and back, and the page shows the level as it goes.
3. The better curve is the one where the sweep looks like an even fade. A
   curve that is too shallow spends the sweep looking bright and then drops
   off at the end; one that is too steep dives to dim early and lingers.

Setting any level ends a sweep. Gamma is saved with the other settings, so the
curve you choose is the one the board keeps. The default is `DIM_GAMMA_X10` in
`src/config.h`, in tenths.

## What 100% means

The most current the panel's controller can drive, which is **brighter than
before** on both panel types. Neither controller's setup ran at full current:

| panel | before | 100% | default setting |
| :-- | :-- | :-- | :-- |
| SSD1327 (grey) | contrast `0x80` of `0xFF`: 50% of full | `0xFF` | 73% |
| SSD1351 (colour) | channels `C8 80 C8` of `FF`: 78% of full | channels `FF A3 FF` | 90% |

The default setting is the one that lands on "before" at gamma 2.2, so an
unconfigured board looks as it always did. On the colour panel, 100% scales
all three channels together, so the colour balance never changes, only the
level. Brighter than the default costs power, and OLEDs wear with brightness,
so the eyes burn in faster. A setting below the default buys panel life for
nothing.

## Trim

Two panels rarely match, even from one batch. The trim takes one panel up or
down by up to 50% of its level, and it stays trimmed at every setting.
Like `flip`, it belongs to the **panel**, not the eye: it is stored against
the chip select, so `swap` moves the eyes and leaves the trim where it is.
Trimming a panel up cannot take it past its own maximum, so at 100% the
brighter panel is the one to trim down.

## Sleep

Sleep mode's level is **a share of the setting**, not of the panel: with the
setting at 60% and a sleep level of 50, the eyes run at 30% overnight. Level 0
is still off. Waking fades back to the setting.

## How it works

**In the panel controllers, not in the pixels.** Both controllers set how much
current drives their pixels, and an OLED's light, and most of its power, follow
that current. Dimming there costs one command per panel, nothing per frame, and
keeps every shade the eye design has. The alternative, scaling each pixel before
it is sent, would save power too, but would take the greyscale panel's 16 levels
down to 4 at 25%, and the eyes would visibly posterise.

- **SSD1327**: one register, `0x81`, current in 256 steps.
- **SSD1351**: two stages. Master current (`0xC7`) is sixteen coarse steps,
  and each colour channel (`0xC1`) has 256 fine ones. The dimmer takes the
  smallest master step that reaches the level and makes up the rest in the
  channels. That keeps fine steps all the way down, where the master alone
  gives sixteen jumps and collapses everything below about 7% into one.

**Off is off.** At 0% the dimmer sends the panel's own display-off command.
The panel keeps its picture and stops driving its rows, and the render loop
stops drawing until the brightness comes back up. That is where the power
saving is largest.

**Fades** take `DIM_FADE_MS` (500 ms) across the whole range, and less for
smaller changes. They move through the setting rather than through the light,
so a fade looks even at any curve. A card (the startup splash, the address
cards, update progress) is shown at the setting at once, or at the default if
the setting is off, because a card is shown to be read. The eyes fade back
afterwards.

**What has not been measured** is how much power it saves. The ESP32 with its
radio always on draws a steady 100–150 mA that no dimmer touches; the panels
are the part that shrinks. A USB power meter and `dim sweep` would give real
numbers, and they belong here when somebody has them.
