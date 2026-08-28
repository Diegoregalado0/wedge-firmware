# Wedge

Firmware and backend for a connected always-on display appliance. The device is
a mains-powered desk unit that shows a clock and delivers short text messages
pushed from a web service, on a 536x240 AMOLED panel driven by an ESP32-S3.

The clock is the resting state and the reason the unit stays powered. Message
delivery is the function it exists to provide.

This repository contains three things that ship together: portable rendering
and application code, an ESP-IDF port, and a Next.js service. The firmware and
the host simulator compile the same `core/` sources, so behaviour evaluated on
a workstation is the behaviour that ships.

## Hardware

Reference target is a LilyGO T-Display-S3 AMOLED 1.91" with capacitive touch.

| Function | Part | Interface |
| --- | --- | --- |
| Display | RM67162, 536x240 | QSPI at 75 MHz, D0-D3 on 18/7/48/5, SCK 47, CS 6, RST 17, TE 9 |
| Touch | CST816T | I2C, SDA 3, SCL 2, IRQ 21 |
| Button | single tactile | GPIO 0, shared with the boot strap |
| Panel power | PMIC enable | GPIO 38 |
| Battery sense | ADC | GPIO 4 |
| SoC | ESP32-S3 | 240 MHz, 8 MB octal PSRAM, 16 MB flash |

There is no ambient light sensor on this variant. The 1.47" Lite carries a
CM32181; the 1.91", Plus and T4-S3 do not. Brightness is therefore driven from
the clock rather than from measured lux.

## Repository layout

```
core/           Portable C99. No platform headers.
  include/      Public API.
  src/          Canvas, text, springs, scene, message model, state machine, faces.
    fonts/      Generated glyph atlases. Regenerate with `make fonts`.
platform/
  sdl/          Host simulator. Same core sources against SDL2.
  esp32s3/      ESP-IDF project: panel, touch, Wi-Fi, NVS, HTTP, OTA.
server/         Next.js service and sender UI. Deploys to Vercel.
tools/          Font baker, headless renderer, regression harnesses, release script.
```

## Building

### Simulator

```sh
make            # build/wedge-sim
make run
```

| Key | Action |
| --- | --- |
| `1`-`5` | inject a sample message |
| `space` | GPIO 0 button |
| `L` | long press, opens diagnostics |
| `[` `]` | step the clock by an hour |
| `S` | write a screenshot |
| drag | grab and dismiss an open message |

### Headless rendering

```sh
make shots
```

Writes `build/shots/`: one PNG per hour and state, a contact sheet, and a 30 fps
capture of a message opening and being dismissed. Used for pixel-level
regression comparison between builds.

### Firmware

```sh
. ~/esp/esp-idf-v5.5.4/export.sh
cd platform/esp32s3
idf.py menuconfig       # Wedge menu: backend URL, device ID, device token
idf.py build flash monitor
```

The device token belongs in the local `sdkconfig`, which is gitignored.
`sdkconfig.defaults` carries only the service URL and device ID.

Reading the console: do not assert DTR or RTS. On this board's native
USB-Serial/JTAG that drops the part into download mode, which is silent and
indistinguishable from a hung device.

### Service

```sh
cd server
npm install
npm run dev             # unprovisioned: state lives in an in-process map
npm test                # 46 assertions, no network, no mocking
vercel deploy --prod
```

Storage backend selects itself: Upstash Redis when `KV_REST_API_*` is present,
Vercel Blob when `BLOB_READ_WRITE_TOKEN` is, an in-process map otherwise. The
last case is what makes the route handlers directly testable.

## Provisioning

With no stored credentials the device raises a SoftAP named `Wedge Setup XXXX`
and serves a captive portal. iOS and Android open it automatically on joining;
otherwise any HTTP address reaches it. The portal scans and lists networks,
hides the password field for open ones, and offers manual SSID entry.

Credentials are written to NVS only after they have been tested against the real
network, so a typo leaves the portal up rather than committing the device to a
network that does not exist. Association is retried internally before failure is
reported, and failures surface the radio's own reason code as plain text.

Two recovery paths exist because the primary one is not reachable in an
enclosure:

- Holding GPIO 0 for ten seconds erases the stored network. The panel warns from
  four seconds and releasing cancels.
- A station that cannot find its network for ten minutes raises the setup AP by
  itself, without erasing anything. The station keeps retrying underneath, so an
  outage that resolves heals with no intervention.

### Captive portal relay

Networks that withhold traffic pending a web sign-in cannot be satisfied by a
device with no browser, and a phone signing in on its own connection authorises
only the phone. When a plain-HTTP reachability probe indicates interception, the
device raises an AP, hands out the upstream resolver, and enables NAPT so a
phone's traffic egresses through the device's own station interface. The portal
then sees the device, and completing the sign-in authorises it. The AP is taken
down automatically once a poll succeeds.

## Over-the-air updates

Two 3 MB OTA slots with bootloader rollback enabled.

```sh
WEDGE_PASSWORD=... tools/release.sh
```

Builds, verifies the version embedded in the image matches `git describe`,
refuses a dirty tree, and publishes. Devices check daily, install to the
inactive slot, and restart when nothing is on screen.

A freshly written image boots as pending. It is marked valid only after it has
reached the backend, which is the narrowest useful definition of working: radio,
TLS, credentials and route all functioning. An image that cannot do so is
restarted out of after five minutes and the bootloader reverts to the previous
slot. The image is verified twice, once by `esp_https_ota` and again against the
manifest's SHA-256 recomputed from flash before the slot is made bootable.

## Wire protocol

| Method | Path | Caller | Auth |
| --- | --- | --- | --- |
| `GET` | `/api/device/messages` | device, 5 min interval | device bearer token |
| `POST` | `/api/device/messages/{id}/read` | device | device bearer token |
| `GET` | `/api/device/ambient` | device | device bearer token |
| `GET` | `/api/device/firmware` | device, daily | device bearer token |
| `GET` `POST` | `/api/messages` | sender | session cookie |
| `DELETE` | `/api/messages/{id}` | sender | session cookie |
| `GET` `PUT` | `/api/ambient` | sender | session cookie |
| `GET` `POST` | `/api/firmware` | sender | session cookie |
| `POST` | `/api/auth` | sender | password |

Credentials are separate by design: the device token authenticates the appliance
and is revocable without affecting sender access, and the sender password never
reaches the device.

Messages carry three observable states. `delivered_at` is stamped when the
device first fetches a message, which distinguishes queued from delivered;
`read_at` when the device acknowledges display.

Scheduling is enforced on the device rather than by withholding server-side. A
message due at 21:00 is delivered ahead of time and held locally, so it still
appears on schedule if the network is unavailable at that moment.

Text is normalised to the panel's character set on ingest. The glyph atlases
cover ASCII 32-126; mobile keyboards substitute U+2019 for an apostrophe, which
is three bytes of UTF-8 that the renderer would otherwise drop.

## Rendering

A software compositor renders into a 536x240 XRGB8888 buffer in PSRAM, converts
to RGB565 with an ordered dither, and pushes to the panel in bands through
double-buffered DMA. There is no LVGL and no third-party graphics dependency.

Frame budget at 240 MHz:

| State | Frame time |
| --- | --- |
| Clock face | 71 ms |
| Message card, animating | 105 ms |
| Message card, settled | 192 ms |

The panel cannot be fed from PSRAM. `esp_lcd`'s SPI backend does not set
`SPI_TRANS_DMA_USE_PSRAM`, so a framebuffer there causes the driver to attempt a
257 kB bounce allocation in internal RAM, which fails on a part with 185 kB of
it. Frames cross in bands from an internal buffer instead; the dither is keyed
to absolute row so band seams do not appear.

### Burn-in mitigation

An AMOLED ages where it is lit, and the clock is static high-contrast content at
a fixed baseline. The composited frame is displaced around a 7x7 lattice, one
pixel per step, advancing on the minute boundary so the motion coincides with
the digits changing. Displacement is applied at the point the frame is handed to
the panel rather than in layout, so no UI element can be omitted from it.

This spreads edges rather than reducing total luminance. Over a day the columns
under an 11 px stroke that are lit continuously fall from 11 to 5; a stroke no
wider than the lattice is never continuously lit. Peak duty is unchanged, and
`make test-shift` asserts that explicitly. Reducing the total requires
brightness reduction or off-time.

## Testing

```sh
make test               # core: moon path, timezone, conversion, pixel shift
cd server && npm test   # 46 assertions against the route handlers
```

The core suites are regression harnesses for behaviour that is expensive to
verify by eye: celestial path continuity across the midnight and dawn seams,
US Pacific daylight-saving transitions to the minute across two years, the
RGB565 conversion checked exhaustively against the arithmetic it replaced, and
the burn-in walk's coverage and step-size properties.

The service tests exercise the real route handlers with no mocking, which the
in-process storage backend makes possible.

## Behaviour notes

Timekeeping is US Pacific with daylight saving applied by rule. The last known
time is persisted to NVS and restored at boot, with the firmware build time as
the fallback on a device that has never synchronised, so the panel never shows
a 1970 timestamp.

Messages do not interrupt. An arriving message raises an indicator and waits.
One press opens it and one press closes it; an unattended message returns to the
clock after 60 seconds. Once read, a message is retained internally for twelve
hours but is not reachable again from the device.

Loss of network is not an error state. The clock remains correct and cached
messages remain available; the only change is a status word in the corner.
