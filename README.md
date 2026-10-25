# ESP32PWM — servo scene controller

Animates a scene of figurines with many hobby servos (MG90S by default) from an ESP32.
Every servo moves between a start and a stop angle with its own duration, delay,
velocity profile and repeat mode, all driven by one shared scene clock so the
choreography stays in sync. You set it up from a phone or laptop over the ESP's own
WiFi access point, or over USB serial with FluidNC-style `$` commands.

- Uses every on-chip PWM block: LEDC high-speed (8), LEDC low-speed (8), MCPWM0 (6), MCPWM1 (6)
- Optional PCA9685 I²C boards, 16 servos each (up to 4 boards)
- Profiles: `const`, `trap` (adjustable acceleration share), `tri`, `sine`, `bell` (minimum jerk)
- Global Stop/Go via a push button, the web UI or commands; Stop/Go per servo
- A servo that was stopped blends smoothly back into the running choreography
- Settings are stored in flash and applied at boot
- The web UI is mobile-friendly and works offline (no CDN); it doubles as a captive portal

## Hardware

Target: ESP32 (classic) DevKit with a WROOM module, 4 MB flash.

| Function | Default pins |
|---|---|
| Servos 0–15 | GPIO 2, 4, 5, 13, 14, 15, 16, 17, 18, 19, 23, 25, 26, 27, 32, 33 |
| Servos 16–31 | PCA9685 board 0 (0x40), channels 0–15 |
| I²C (PCA9685) | SDA GPIO 21, SCL GPIO 22, 400 kHz |
| Stop/Go button | GPIO 0 = the DevKit's **BOOT** button (active low, internal pull-up) |
| Serial console | UART0 (USB), 115200 baud |

Pins are settings, so you can change any of them (see below). GPIO 6–11 (flash),
1/3 (console) and the input-only GPIO 34–39 can't drive servos. On WROVER modules,
GPIO 16/17 are taken by PSRAM. An external button can go on any free GPIO,
e.g. `$/button/pin=gpio.34`; GPIO 34–39 have no internal pull-up, so fit a 10 kΩ resistor to 3.3 V.

**Power.** Never run servos from the ESP32's 5 V pin.
- **Supply:** a separate 5–6 V supply. Plan about 0.3 A per moving MG90S, and up to 1 A each at stall. Stronger servos need more.
- **Ground:** connect the supply ground to the ESP ground.
- **Capacitors:** fit 470–1000 µF near each group of about 8 servos.
- **Signal level:** 3.3 V signals drive MG90S servos fine. If a stronger servo ignores them, add a 74AHCT125 buffer.

At boot, servos are switched on one after another (`$/scene/stagger_ms`) to avoid a current spike.

## Build, flash, test

PlatformIO with the ESP-IDF framework (`espressif32@8.0.0`, ESP-IDF 6.1.0, GCC 14.2).

```sh
pio run                      # build (also writes firmware-signed.bin for OTA)
pio run -t upload -t monitor # flash and open the serial console
pio test -e native           # unit tests on the PC (motion math, scene engine, parser)
python3 tools/sim_server.py  # web UI against a desktop simulator: http://localhost:8080/
```

If `pio` is not on your PATH, it is usually `~/.platformio/penv/bin/pio`.
For the first run on real hardware, follow [docs/hardware-checklist.md](docs/hardware-checklist.md).
PlatformIO generates `sdkconfig.esp32dev` only once; after editing `sdkconfig.defaults`,
delete it so it is regenerated.

## Over-the-air updates

The ESP only installs firmware that is **signed with the project key**.
- **Signature:** ECDSA P-256, in the format of Espressif's `espsecure.py sign_data --version 1`.
- **Serial flashing is not affected.** No eFuses or Secure Boot are involved, so `pio run -t upload` always works, with any image.
- **Rollback:** a new firmware has to run for 10 s before it is confirmed. If it crashes or resets earlier, the bootloader switches back to the previous one.
- **During the update:** the scene holds while flash is written. If the update fails, the scene resumes.

**Keys**

| File | Role |
|---|---|
| `~/.config/esp32pwm/ota_signing_key.pem` | Private key. Stays outside the repo. **Back it up.** Set a different path with `custom_ota_signing_key` in `platformio.ini` or `$ESP32PWM_OTA_KEY`. |
| `keys/ota_verify_key.bin` | Public key, committed. Embedded in the firmware and refreshed from the private key on every build. |

Every `pio run` writes two files to `.pio/build/esp32dev/`:
- `firmware.bin`: unsigned, used for serial flashing
- `firmware-signed.bin`: signed, used for OTA

To create a new key, use any Python with the `cryptography` package, e.g. ESP-IDF's environment:

```sh
~/.platformio/penv/.espidf-6.1.0/bin/python scripts/ota_sign_tool.py genkey ~/.config/esp32pwm/ota_signing_key.pem
```

`scripts/ota_sign_tool.py` can also `sign` and `verify` images by hand.

Then flash once over serial, because the firmware embeds the matching public key.
If the key is lost, you can still flash over serial, then use a new key.

**Updating from your computer (pull):**
1. Join the ESP's WiFi with the computer.
2. Start the server: run the VS Code task **OTA: serve firmware (port 9999)**, which builds first. Or run `python3 tools/serve_firmware.py`.
3. In the web UI open Settings → Firmware and press **Update from server**. Or send `$Firmware/Update` over serial.

The address field is prefilled with the first client that joined the access point. The ESP fetches
`http://<that address>:9999/firmware.bin`. `$Firmware/Update=<url>` lets you choose any URL.
Your firewall must allow incoming TCP on port 9999 (e.g. `sudo firewall-cmd --add-port=9999/tcp`).

**Uploading from the browser (push):** Settings → Firmware → **Upload .bin…** and choose
`firmware-signed.bin` (`POST /update`).

`$I` shows the running version and slot, and what the other slot holds.

## First use

1. Power up and join the WiFi network **ServoScene-XXXX**. The password is `servoscene`.
2. Most phones open the UI by themselves (captive portal). Otherwise browse to <http://192.168.4.1/>.
3. Change a value and the servo reacts at once. Press **Save** to keep your changes over a restart.
   If a change needs a restart (pins, frequencies, WiFi), a banner offers **Save & restart**.

The **Servos** tab has one card per servo. Each card has:
- a Stop/Go button for that servo
- start/stop angle sliders, timing, profile and mode
- a plot of the servo's angle over one scene cycle; the line is the current scene time and the dot is the real output

**Timeline** shows every servo on one time axis. **Calibrate** moves one servo directly
and sets its pulse range. **Settings** covers everything else, plus config backup and
restore. **Console** is the same command line as the serial port.

## Motion model

All servos follow one **scene clock**. A servo's target angle depends only on the scene
time and its own settings:

| Mode | Behaviour |
|---|---|
| `return` (default) | Every scene cycle: wait `delay_ms`, move start→stop in `duration_ms`, wait `dwell_ms`, move back. |
| `once` | Waits `delay_ms` after the scene starts, then moves start→stop once and holds. |
| `pingpong` | Moves start↔stop continuously, with `dwell_ms` at both ends, independent of the cycle. |

The scene cycle is `$/scene/period_ms`. If that is `0`, the cycle is the longest
servo's delay + move + dwell + return.

`duration_ms` is always the full move time; the profile only shapes the speed along the way.
Peak speed compared with a constant-speed move: `const` 1×, `trap` 1/(1−a) where a = `accel_pct`/100,
`tri` 2×, `sine` 1.57×, `bell` 1.875×. If a move would exceed `max_dps`, you get a warning.

- **Global Stop** (button, `!`, header button) freezes the scene clock. Every servo holds its position, and **Go** carries on exactly where it stopped.
- **Per-servo Stop** holds just that servo while the scene keeps running. On **Go** the servo *rejoins*: it blends from where it is onto its still-moving target with a minimum-jerk curve. The blend lasts at least `$/scene/rejoin_ms` and is lengthened if needed to stay under `max_dps`. When it finishes, the servo is exactly back in sync.
- **Other rejoins:** the same blend is used after a scene reset, after test mode, after re-enabling a servo, and after editing its motion while running. So no edit makes a servo jump.

## Commands (serial and WebSocket)

Syntax follows FluidNC. Each command line is answered with `ok`, or with `error:N`
plus a `[MSG:ERR: reason]` line before it. Realtime characters act at once and get no `ok`.
Over serial, `!` `~` `?` are ordinary text inside a value (after `=`), so names and passwords
may contain them; Ctrl-X always acts at once. Over WebSocket a realtime command is a frame
holding just that character.

| Command | Meaning |
|---|---|
| `!` / `~` | Global Stop / Go |
| `?` | Status, e.g. `<Run\|T:12.340\|Cyc:3\|Stop:3,7\|Rejoin:5\|Test:2>` |
| Ctrl-X | Scene reset to t = 0 (servos rejoin smoothly) |
| `$` or `$H` | Help |
| `$$` / `$S` | List settings; `$S=/servo/3` lists one subtree |
| `$/path` | Query one setting or subtree, e.g. `$/servo/3`, `$/scene` |
| `$/path=value` | Change a setting (applies at once, not yet saved) |
| `$CS` / `$Save` / `$CD=config` | Save the changed settings to flash |
| `$CD` | Show the whole config as YAML |
| `$RST=*` | Factory defaults (also erases the saved config) |
| `$I` | Version, PWM block usage, PCA boards found, servo → hardware map, startup problems |
| `$Bye` | Restart |
| `$Scene/Stop`, `$Scene/Go`, `$Scene/Reset` | Same as `!`, `~`, Ctrl-X, as text commands |
| `$Servo/Stop=n` / `$Servo/Go=n` | Stop or resume one servo (`*` = all) |
| `$Servo/Test=n,deg` | Drive servo n to an angle at up to `max_dps` (calibration) |
| `$Servo/Release=n` | Leave test mode and rejoin the scene (`*` = all) |
| `$Positions` | Current output angles, e.g. `[POS:0=45.0,1=90.5]` |
| `$Firmware/Update[=url]` | Download and install signed firmware (default `http://<first client>:9999/firmware.bin`; `http://` only); progress as `[OTA:download 40%]` |

Error codes: 2 bad value, 3 unknown command, 4 out of range, 5 unknown setting,
6 servo not enabled, 7 flash write failed.

### Settings

Paths are case-insensitive. "R" means the change applies after a restart (`$Bye`).

**`/servo/<0..63>/…`**

| Field | Default | Range / values | |
|---|---|---|---|
| `name` | empty | up to 15 characters | |
| `pin` | see Hardware | `none`, `gpio.N`, `pca.<board>.<channel>` | R |
| `driver` | `auto` | `auto`, `ledc`, `mcpwm` (on-chip block for GPIO servos) | R |
| `enable` | `true` | `true`/`false` | |
| `start_deg`, `stop_deg` | 45, 135 | 0–360 | |
| `delay_ms` | servo × 250 | 0–3600000 | |
| `duration_ms` | 1500 | 0–600000 | |
| `dwell_ms` | 500 | 0–600000 | |
| `profile` | `bell` | `const`, `trap`, `tri`, `sine`, `bell` | |
| `accel_pct` | 25 | 0–50, only used by `trap` | |
| `mode` | `return` | `once`, `return`, `pingpong` | |
| `min_us`, `max_us` | 500, 2400 | pulse width at 0° and at `range_deg` | |
| `range_deg` | 180 | 1–360, the servo's mechanical range | |
| `trim_us` | 0 | −500–500 | |
| `invert` | `false` | mirror the direction | |
| `max_dps` | 600 | speed limit in °/s, 0 = none (MG90S ≈ 600 °/s at 4.8 V) | |

**Other groups**

| Path | Default | |
|---|---|---|
| `/scene/period_ms` | 0 (auto) | |
| `/scene/autostart` | `true` (start in Go) | |
| `/scene/update_hz` | 50 (motion task rate, 10–200) | R |
| `/scene/rejoin_ms` | 600 | |
| `/scene/stagger_ms` | 50 | |
| `/pwm/ledc_freq_hz`, `/pwm/mcpwm_freq_hz` | 50 (40–1000; digital servos may accept more) | R |
| `/i2c/sda`, `/i2c/scl`, `/i2c/khz` | `gpio.21`, `gpio.22`, 400 | R |
| `/pca/<0..3>/enable`, `addr`, `freq_hz`, `osc_hz` | board 0 on at 0x40, 50 Hz, 25000000 | R |
| `/button/pin`, `active_low`, `pullup` | `gpio.0`, `true`, `true` | R |
| `/button/debounce_ms` | 30 | |
| `$AP/SSID` | empty = `ServoScene-XXXX` | R |
| `$AP/Password` | `servoscene` (empty = open network, else 8–63 characters) | R |
| `$AP/Channel`, `$AP/IP`, `$AP/Captive`, `$Hostname` | 6, `192.168.4.1`, `true`, `servoscene` | R |

**Pulse accuracy.** The PCA9685's internal oscillator is only accurate to a few percent.
Measure a pulse with a scope or logic analyzer and set `osc_hz` so the widths come out right.

**Resolution at 50 Hz:**
- LEDC: ≈0.3 µs (16 bit)
- MCPWM: 0.3125 µs (3.2 MHz timer clock)
- PCA9685: ≈4.9 µs (12 bit), about 0.5° on an MG90S

### Saved format and backup

Only settings that differ from their defaults are saved, as `$/path=value` lines.
At boot they are replayed through the same parser, and any line that fails is reported
by `$I`. `GET /config.txt` downloads this text and `POST /config.txt` restores it
(Settings → Backup in the UI). Press Save afterwards to keep a restored config.

## Project layout

```
src/core/      portable logic, unit-tested on the PC
  Config       configuration structs and defaults
  Motion       profiles, target angle over scene time, angle → pulse
  Scene        scene clock, per-servo stop, rejoin blend, test mode
  Settings     settings tree: paths, types, ranges, save format
  Parser       FluidNC-style command parser (serial, WebSocket, boot replay)
src/out/       LEDC / MCPWM / PCA9685 drivers and the channel allocator
src/app/       shared state and lock, 50 Hz motion task + button, command executor, NVS
src/net/       WiFi access point, web server + WebSocket, captive DNS, OTA + signature check
web/           UI (embedded gzipped at build time by scripts/embed_web.py)
keys/          public OTA verification key (scripts/ota_sign.py keeps it in sync and signs builds)
test/          native unit tests
tools/         desktop simulator + UI server, OTA firmware server
.vscode/       tasks: OTA build + serve, web UI simulator
```
