# Wedge

A bedside appliance that shows the time and, when there is one, hands over a
message from someone who loves her.

The clock is why it earns a place on the table. The messages are the product.

Hardware is a LilyGO T-Display-S3 AMOLED 1.91" (touch), 536 x 240, in a printed
landscape wedge reclined 54 degrees from vertical.

## Layout

```
core/         Portable C. No platform headers anywhere in here.
  include/    Public headers.
  src/        Canvas, text, springs, scene, message model, state machine, faces.
    fonts/    Generated glyph atlases. Do not edit; run `make fonts`.
platform/
  sdl/        Host simulator. Same core sources, SDL2 instead of a panel.
  esp32s3/    ESP-IDF project. RM67162, CST816T, Wi-Fi, NVS, HTTP client.
server/       Next.js backend and the sender interface. Deployed on Vercel.
tools/        Font baker and the headless frame renderer.
```

The device firmware and the simulator compile the same `core/` sources. They
cannot drift, and anything judged in the simulator is the thing that ships.

## Build and run the simulator

```sh
make          # builds build/wedge-sim
make run
```

| Key | Does |
| --- | --- |
| `1`-`5` | queue a sample message |
| `space` | the GPIO 0 button |
| `L` | long press, opens diagnostics |
| `[` `]` | step the clock an hour, to watch the sky and brightness follow |
| `S` | write a screenshot |
| drag | grab an open message and throw it away |

## Review the design without a window

```sh
make shots
```

Writes `build/shots/`: one PNG per hour and state, a contact sheet, and
`motion.gif`, a 30 fps capture of a message opening, resting, and being flicked
away. Stills cannot show whether a spring settles well.

## Build the firmware

```sh
. ~/esp/esp-idf-v5.5.4/export.sh
cd platform/esp32s3
idf.py menuconfig       # Wedge menu: backend URL, device token
idf.py build flash monitor
```

The device token belongs in the local `sdkconfig`, which is gitignored.
`sdkconfig.defaults` carries only the URL and device id.

## Setting up Wi-Fi

Leave the SSID unset and the device asks for it on first boot. No app, no cable.

1. The panel says **Set up** and shows a network name, `Wedge Setup XXXX`.
2. Join that network from the phone's Wi-Fi settings.
3. iOS opens the page by itself. If it does not, visit any address.
4. Pick the house network from the scanned list, type the password, Connect.
5. The device tests the credentials before storing them, then restarts.

Credentials are written only after they have actually connected, so a typo
leaves the portal up rather than bricking the device into a network that does
not exist. To change networks later, hold the button for ten seconds: the panel
warns from four seconds in, and releasing before then cancels.

## The backend

```sh
cd server
npm install
npm run dev             # runs unprovisioned, messages live in memory
vercel deploy --prod
```

Storage picks itself: Upstash Redis if `KV_REST_API_*` is set, Vercel Blob
otherwise, an in-process map when neither is. No code changes to move between
them.

### Wire protocol

| Method | Path | Who | Auth |
| --- | --- | --- | --- |
| `GET` | `/api/device/messages` | device, every 5 min | device bearer token |
| `POST` | `/api/device/messages/{id}/read` | device | device bearer token |
| `GET` `POST` | `/api/messages` | sender | session cookie |
| `DELETE` | `/api/messages/{id}` | sender | session cookie |
| `POST` | `/api/auth` | sender | password |

The two credentials are separate on purpose: the device token authenticates the
appliance and can be revoked without touching the sender's access, and the
sender's password never reaches the device.

Scheduling is enforced on the device, not by withholding on the server. A
goodnight message is delivered hours early and held locally, so it still appears
at 21:00 if the network is down at 21:00.

## How it behaves

Five scheduled modes run on the clock: sleep, morning, day, evening, night.
They set brightness and nothing else switches on their boundaries. The sky is a
continuous function of the hour, so dusk arrives by the color actually changing
over an hour rather than by a scene being swapped at 17:00.

A message never interrupts. It raises an indicator that breathes, and waits.
Touching the glass opens it; the button does the same thing for anyone who
would rather press something. An open message returns home on its own after 25
seconds, or when it is flicked down.

Offline is not an error state. The clock is still true and the cached messages
are still hers, so the only thing that changes is a small word in the corner.

## Notes on the hardware

There is no ambient light sensor on this board. The 1.47" Lite variant carries a
CM32181; the 1.91" does not, and neither does the Plus or the T4-S3. Brightness
is therefore a hand-drawn curve over the clock, floored at 6/255 during sleep
hours, and the sleep face is mostly black pixels, which on an AMOLED means
genuinely off.

| Function | Part | Pins |
| --- | --- | --- |
| Display | RM67162, QSPI 75 MHz | D0-D3 18/7/48/5, SCK 47, CS 6, RST 17, TE 9 |
| Touch | CST816T, I2C | SDA 3, SCL 2, IRQ 21 |
| Button | one | GPIO 0 |
| Panel power | PMIC enable | GPIO 38 |
| Battery sense | ADC | GPIO 4 |
