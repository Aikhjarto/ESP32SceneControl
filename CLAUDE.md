# CLAUDE.md — working notes for this repository

ESP32 (classic) firmware that animates figurines with many servos and digital outputs on
one shared scene clock. Configured over the ESP's WiFi access point (web UI) or USB serial
with FluidNC-style `$` commands. User-facing documentation is [README.md](README.md);
this file holds the decisions, rules and state a new session needs to continue the work.

The project is called **ESP32SceneControl** (renamed from ESP32PWM on 2026-10-27; the
checkout folder may still have the old name). The OTA update refuses images whose ESP-IDF
project name differs from the running one, so a board that runs a build from before the
rename needs one serial flash. The WiFi defaults followed: AP `SceneControl-XXXX`, hostname
and password `scenecontrol`. Only the internal NVS namespace stays `servoscene`, so saved
settings survive.

## Working rules (from the user)

- **Ask before installing anything:** any platform, toolchain, tool or pip package,
  including PlatformIO reinstalling its own Python deps, or a different version of one.
- **Commits:** commit only when asked. Approved pattern: one commit per milestone or per
  group of review fixes, and only once it builds and the native tests pass.
- **Reviews:** don't start a new review agent unless the user asks for one. When the user
  asks for a review, fix all its findings unless they say otherwise.
- **OTA signing key:** `~/.config/esp32scenecontrol/ota_signing_key.pem` (or `$ESP32SCENECONTROL_OTA_KEY`)
  stays outside the repo and is never committed or printed. Only the public key
  `keys/ota_verify_key.bin` is in git. On a new machine the user copies the .pem securely.
- **Simulator:** the user often has their own simulator running on port 8080 with
  `.pio/sim/sim-config.txt`. Don't touch either. For your own testing, use another port and
  a config in your scratchpad: `python3 tools/sim_server.py --port 8791 --config <scratch>/sim.txt`.
- Pronouns for the user: they/them.

## Decisions and their reasons

- **Classic ESP32 only** (WROOM, 4 MB). No S3, C3 or C6 environments.
- **Servos are MG90S for now, maybe stronger later.** So calibration is per servo (pulse
  min/max, angle range) and nothing is hard-coded.
- **One scene clock, no per-servo clocks.** Every servo's target is a pure function of the
  scene time, so the choreography stays in sync. The user confirmed this design over
  per-servo clocks.
- **Stop/Go:**
  - Global Stop pauses in place: the clock freezes and positions and output states hold.
  - The button is GPIO0 (the BOOT button), so an unwired pin can't float.
  - Every servo card and every output has its own Stop/Go, overridden by the global Stop.
  - A servo that is restarted blends ("settles") back onto its moving target.
- **Settle** (`Scene::startRejoin`): a minimum-jerk blend from where the servo was to its
  moving target, plus the run-on of its current speed (`speedRunOn` in Motion.h), all
  within about 2000 °/s².
  - Reset, Release, Go and re-enable start from rest.
  - Edits and period changes keep the current speed.
  - An edit that doesn't change where the servo is going starts no settle at all.
  - There is no hard range clamp: an earlier clamp stalled fast servos. The run-on time is
    shortened instead (sized against the room the blend leaves free, kRoomFactor), so the
    servo stays inside its start..stop range. One outside its new range doesn't run on.
- **Timeline:** a looped cycle with a per-servo mode (once / return / pingpong), delay and
  dwell. The automatic period also covers the outputs' last switch point, rounded up to
  the next full second. Firmware (`effectivePeriodMs`) and `web/app.js` must agree.
- **Digital outputs:** named, with switch-point lists within the cycle. A point's state is
  on, off or a blinking pattern (blink, flash, flicker, heartbeat; the user chose all four
  and "pattern as a state in the sequence" over one pattern per output). A pattern starts
  at its switch point, so it stays in sync. The flicker is a deterministic hash of output,
  point and slot, which `web/app.js` mirrors exactly (`Math.imul`).
  - `seq` stays 200 bytes: longer would push the worst-case saved config past 64 KB.
  - Hardware: free GPIOs or PCF8574/PCF8575 bits (`pcf.B.C`).
  - The invert setting is latched at boot for the pin it was booted with, so `$RST=*`
    can't turn active-low outputs on.
  - A PCF board missing at boot is probed again every 5 s (timed, not counted in ticks).
  - Before a software restart, the motion task switches every output off (shutdown handler).
  - Points after the end of a fixed period are never reached and set no state, also not the
    wrap state (`sequenceState` and `seqState` in app.js).
- **Storage:** an NVS blob of only the non-default settings, as `$path=value` lines,
  replayed through the parser at boot. A restore is validated in a scratch Config first.
  - The worst case is about 49 KB, hence the 128 KB NVS partition and the 64 KB upload limit.
- **Text settings** (names, SSID, ...) must be UTF-8 without control characters, and every
  WebSocket text frame is made valid UTF-8, or the browser drops the connection.
- **Calibration edits** on a powered servo glide the pulse at `max_dps` (`PulseGlide`).
- **No mDNS:** the UI is at 192.168.4.1, with a captive-portal DNS.
- **OTA:**
  - Only signed images are accepted: ECDSA P-256 in espsecure v1 format, checked in the
    app with PSA Crypto. A held header byte prevents an unverified image from booting.
  - Rollback is enabled.
  - Serial flashing must always stay possible, so there is no Secure Boot and no eFuse
    burning.
  - Pull update: the UI fetches `http://<first AP client>:9999/firmware.bin`. That file is
    served by `tools/serve_firmware.py`, started by a VS Code task.

## Layout

- `src/core/`: portable code, also built for the native tests and the simulator.
  - Config, settings tree and Settings
  - Parser (the shared command parser)
  - Scene (the engine)
  - Motion (profiles and settle math)
  - Sequence (output switch points)
  - LineReader
- `src/app/`: App (boot), MotionTask (tick and hardware writes), Commands, Persist (NVS).
- `src/out/`: Outputs (LEDC, MCPWM, GPIO and pin allocation), Pca9685, Pcf857x.
- `src/net/`: Wifi (AP), WebServer (HTTP, `/ws`, `/config.txt`), CaptiveDns, Ota, SignedImage.
- `web/`: the UI (`index.html`, `app.js`, `style.css`). `scripts/embed_web.py` gzips it into
  `web/dist/`, which is ignored by git.
- `scripts/ota_sign.py` (PlatformIO extra script) and `scripts/ota_sign_tool.py`: they write
  `.pio/build/esp32dev/firmware-signed.bin`.
- `tools/sim/main.cpp` and `tools/sim_server.py`: a desktop simulator for the web UI.
  It needs aiohttp.
- `test/test_motion`, `test/test_parser`, `test/test_outputs`: Unity tests, 71 in total.
- `docs/hardware-checklist.md`: the bring-up plan for real hardware.

## Build and test

`pio` is usually `~/.platformio/penv/bin/pio`; it is not on PATH.

```sh
pio run                     # firmware + firmware-signed.bin (needs the signing key)
pio test -e native          # 71 host tests; run them before every commit
python3 tools/sim_server.py --port 8791 --config <scratch>/sim.txt   # UI against the simulator
```

- **UI changes:** check them in the simulator with headless Chromium over CDP. The driver
  script used so far lived in the session scratchpad and is not committed.
- **sdkconfig:** `sdkconfig.esp32dev` is generated once from `sdkconfig.defaults`. Delete
  it after editing the defaults.

## Toolchain (versions are pinned on purpose)

- PlatformIO Core 6.2, `espressif32@8.0.0` = ESP-IDF 6.1.0, GCC 14.2.
- ESP-IDF's Python env is usually `~/.platformio/penv/.espidf-6.1.0`.
  - It has `cryptography` but no `ecdsa`, and the platform's esptool ships no espsecure.
    That's why signing uses our own `ota_sign_tool.py`.
- The VS Code PlatformIO extension may recreate penv and wipe pip packages; a rebuild then
  takes a long time. Ask the user before letting it reinstall.

ESP-IDF 6.1 pitfalls hit so far:

- **mbedTLS 4.1:** the legacy crypto API is private. Use PSA Crypto.
- **MCPWM:** capabilities come from `MCPWM_LL_GET` in `hal/mcpwm_ll.h`. The group clock
  defaults to 160 MHz; this project uses a 3.2 MHz timer.
- **WebSocket:** the handshake never reaches the URI handler, so per-connection setup goes
  in `open_fn`.
- **Unborn git HEAD:** the build needs `set(PROJECT_VER ...)` in `CMakeLists.txt`.
- **`embed_web.py`:** a comment with "coding" or "encoding" in its first two lines is read
  as a Python encoding declaration.
- **`dhcps_offer_t`:** needs `dhcpserver/dhcpserver.h`.

## Status (2026-10-27)

- All milestones and six review rounds are done and committed.
  - Verified by the firmware build (55% flash, 30% RAM), 71 native tests and
    headless-browser runs against the simulator.
- **Not yet tested on real hardware:** no ESP32 has been connected. The next step is to
  work through [docs/hardware-checklist.md](docs/hardware-checklist.md) in order.
- **Partition layout:** it changed in 049fa04. A board that ran an older build needs
  `pio run -t erase` and a serial flash.
- **No open review findings.**
