# Configuring the firmware

Two kinds of setting, and it matters which is which.

**Build options** are compile-time: they decide what is in the firmware at
all, and changing one means a rebuild. Every switch is `#ifndef`-guarded, so
it can be overridden from `platformio.ini` without editing a header — with the
exception of the pin numbers, noted below.

**Saved settings** are runtime: the eye design, the timezone, the clock
colours. They live in flash, survive a reboot, and are changed from the
control page or the console.

## Build options

Compile-time switches. Most live in [`src/config.h`](../src/config.h); the ones
inherited from upstream are still declared in `src/main.cpp`, next to the
rendering code they belong to. Either way each is `#ifndef`-guarded, so any of
them can be overridden from `build_flags` in `platformio.ini` without editing
a source file — which is how the `gray` environment sets `USE_SSD1327`.

Two groups are **not** guarded and cannot be overridden this way. Three
derived values — `CONTROLLABLE`, `FAVICON_FRANK` and `FAVICON_EYES` — are
computed from the others. And the **pin numbers** in
[`src/main.cpp`](../src/main.cpp) (`DISPLAY_DC`, `DISPLAY_RESET`,
`SELECT_L_PIN`, `SELECT_R_PIN`, `MOSI_PIN`, `MISO_PIN`, `SCLK_PIN`,
`UART_RX_PIN`) are plain `#define`s: `-DDISPLAY_DC=…` is a macro
redefinition, not an override, so those are edited in place.

| Option | Default | Effect |
| :----- | :------ | :----- |
| `USE_SSD1327` | `0` | Selects the grayscale panel driver. Set by the `gray` environment via `build_flags`, not edited by hand. |
| `COMMANDS` | `1` | Serial console and BOOT-button toggle. `0` compiles both out; the REST API still works, so a network build stays fully controllable. |
| `DEBUG` | `1` | Serial diagnostics and the LED heartbeat, which toggles once a second. `0` saves ~2 KB. |
| `DEBUG_BAUD` | `115200` | Console speed. **Must match `monitor_speed`** in `platformio.ini`. |
| `DEBUG_LED_PIN` | `2` | On-board LED used for the heartbeat. |
| `STARTUP_SPLASH` | `1` | Panel name cards at boot. `0` boots straight into the eyes. |
| `SPLASH_SECONDS` | `5` | How long the splash counts down. |
| `BOOT_BUTTON_PIN` | `0` | Button that toggles eye artwork. |
| `SSD1327_SPI_HZ` | `8000000` | Grayscale bus speed. Lower it if long jumpers cause flicker. |
| `STARTLE_WINDUP_MS` | `1400` | Slow constrict before the startle jolt. |
| `CLOCK` | `1` | Analogue clock face. `0` compiles it out. |
| `CLOCK_*_LEN` / `CLOCK_*_HW` | — | Hand lengths and half-widths, in pixels from the iris centre. |
| `CLOCK_*_COLOR` | `0x000000` | Starting hand colours, changeable at runtime with `clock color`. |
| `CLOCK_NOON` | `128` | Where twelve sits, in the polar table's 0–511 angle. |
| `PUPIL_OFF_SCALE` | `64` | Iris scale used when the pupil is off. At or below 64 the pupil vanishes. |
| `NETWORK` | `1` | WiFi, NTP, web server and OTA. `0` compiles all of it out. With `CLOCK=0` too that is 679 KB, taking a `gray` build from 69% of the partition to 33%. |
| `RTC` | `0` | A DS3231 battery-backed clock. `1` fits one; costs ~26 KB. See [docs/WIRING_RTC.md](WIRING_RTC.md). |
| `RTC_SDA_PIN` / `RTC_SCL_PIN` | `21` / `22` | I²C pins for it. Both otherwise unused. |
| `RTC_ADDR` | `0x68` | The DS3231's fixed address. |
| `FAVICON` | `FAVICON_FRANK` | Tab icon. `FAVICON_EYES` is a generic alternative for a build that is not going into a Frankenstein. |
| `OTA_REBOOT_DELAY_MS` | `1500` | How long after a successful update the board waits before rebooting into it, so the sender hears that it worked. `0` restores the library's behaviour. |
| `OTA_TIMEOUT_MS` | `10000` | How long the board waits for the next block of an over-the-air update. The core's 1000 is shorter than the sender's patience. |
| `AUTH_HTTP` | `0` | Digest authentication on the page, the API and `/cmd`. |
| `AUTH_TOKEN` | `0` | A bearer token as an alternative credential, for scripts. |
| `AUTH_HOST_CHECK` | `0` | Refuse requests whose `Host` is not this device — the DNS-rebinding defence. |
| `SLEEP` | `1` | Dark panels overnight. `0` compiles it out. |
| `SLEEP_ENABLED` | `0` | Whether the window is in force out of the box. Off: a prop going dark unasked reads as a fault. |
| `SLEEP_START_MIN` / `SLEEP_STOP_MIN` | `22*60` / `7*60` | The default window, in local minutes past midnight. |
| `SLEEP_LEVEL` | `0` | 0 switches the panels off and stops rendering; 1–100 dims them and keeps the eyes moving. |
| `SLEEP_WAKE_S` | `60` | How long a command holds the eyes awake inside the window. `0` makes the window absolute. |
| `FACTORY_RESET_MS` | `10000` | How long BOOT must be held, while running, to erase every setting. `0` removes the gesture, and so does `COMMANDS=0`, which is what polls the button. |
| `IPV6` | `0` | All of IPv6, compiled out. Cannot usefully be turned on yet — see [IPv6](NETWORK.md#ipv6). |
| `FIRMWARE_VERSION` | `1.0` | Bumped by hand, for features worth announcing. |
| `PROJECT_URL` | this repository | Shown by `version` and on the control page. |
| `WEB_CMD_ENDPOINT` | `1` | The `/cmd` escape hatch. `0` leaves only the REST API. Needs `COMMANDS`, since it is a passthrough to the console. |
| `WIFI_HOSTNAME` | `frank` | DHCP and mDNS name. |
| `WIFI_AP_NAME` | `frank-setup` | The setup portal's own network name. |
| `NTP_SERVER_1` / `NTP_SERVER_2` | `pool.ntp.org`, `time.nist.gov` | Time servers. A server on your own LAN works here. |
| `NET_SHOW_MS` | `12000` | How long `net` leaves the address cards up. |
| `NET_COLS` | `21` | Characters a 128 px panel fits, for wrapping those cards. |
| `TZ_MAX` | `48` | Longest POSIX timezone string that can be stored. |
| `WIFI_CONNECT_MS` | `15000` | How long to wait on a known network before opening the portal. |
| `WIFI_PORTAL_S` | `60` | How long the portal stays up before carrying on offline. |
| `WIFI_RETRY_MS` | `30000` | How often to try again after giving up at boot. |
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
wire differently, and update [docs/WIRING.md](WIRING.md) to match.

One switch is derived rather than set: `CONTROLLABLE` is `COMMANDS || NETWORK`,
and gates the machinery both interfaces share — the operations layer in
[`src/state.h`](../src/state.h), the settings it persists, and the state the
renderer reads. Turning off *both* interfaces leaves the eyes running on their
own with nothing able to change them, which builds and works but is only
useful if you want a prop with no controls at all.

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
| Time server on or off (`ntp on` / `ntp off`) | |
| Sleep: enabled, window, brightness | |

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

**`forget` also clears any stored credentials**, since they share this
namespace — see [Locking it down](SECURITY.md#forget-does-this-too-and-it-is-reachable-over-the-network).

`status` marks unsaved changes with `(unsaved)`. `forget` clears the stored
settings and the build defaults apply again at the next boot.

Saving is explicit rather than automatic: NVS writes have finite endurance,
and the BOOT button cycles eye designs, so auto-saving would write flash on
every press.

The design is stored **by name**, not by index. Indices shift whenever the set
of `EYE_*` switches changes, so a saved index could silently select a
different design after a rebuild. If a saved design is not in the current
build, the console says so at boot and falls back to the first one.

## Versions

```
> version
frank 1.0 (41038d1), built Sep 10 2026 16:12:04
https://github.com/michael-gebis/creeper-eyes
```

The same three facts are on the control page, under Device, and at
`GET /api/v1/info`.

`FIRMWARE_VERSION` in [`src/config.h`](../src/config.h) is bumped by hand, and
only for something worth telling somebody about — the commit already
distinguishes every build. The commit comes from
[`tools/git_rev.py`](../tools/git_rev.py), which PlatformIO runs before each
build; outside a git checkout it reads `unknown`, and a build made with
uncommitted changes is marked `+dirty`, because an unmarked hash is a promise
that the binary *is* that commit.
