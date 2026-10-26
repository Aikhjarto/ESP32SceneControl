# Hardware bring-up checklist

Everything below has so far been verified only by builds, PC unit tests and a
browser simulator. Work through it in order the first time real hardware is available.
Each item says what to do and what result to expect.

`pio` means `~/.platformio/penv/bin/pio`. Commands go into the serial monitor
(`pio device monitor`) or the web UI's Console tab.

## 0. Preparation

- [ ] **Equipment:**
  - ESP32 DevKit (WROOM, 4 MB)
  - oscilloscope or logic analyzer
  - multimeter
  - a separate 5–6 V servo supply, rated about 0.3 A per MG90S plus headroom
  - 470–1000 µF capacitors
  - optional: PCA9685 board, one MG90S for the first tests
- [ ] **Wiring:** servo supply ground connected to ESP ground. Servos must **not** be powered from the ESP's 5 V pin.
- [ ] **Firewall:** port 9999/tcp is open on the laptop for OTA tests: `sudo firewall-cmd --add-port=9999/tcp`.
- [ ] **Signing key:** `~/.config/esp32pwm/ota_signing_key.pem` exists and is backed up.

## 1. First flash over serial and boot

- [ ] Run `pio run -t upload -t monitor`. It flashes the bootloader (with rollback), the partition table and the app.
- [ ] **Boot log:**
  - [ ] `ESP-IDF v6.1.0` is shown
  - [ ] no panic and no reboot loop
  - [ ] the `allocated: LEDC-HS …, LEDC-LS …, MCPWM0 …, MCPWM1 …` counts add up to 16 GPIO servos
  - [ ] `access point "ServoScene-XXXX"` appears
  - [ ] `web server on http://192.168.4.1/` appears
- [ ] **`$I` output:**
  - [ ] `[VER:0.1.0 ESP32PWM]`
  - [ ] `Firmware 0.1.0 in ota_0`
  - [ ] one `[SERVO:n gpio.X <block>]` line per servo
  - [ ] the only `[MSG:WARN:` line is "PCA9685 board 0 not found", if no board is attached
- [ ] **Servo-to-block mapping:** note which servo sits on which block. By default servo 0 → `ledc-hs`, 1 → `ledc-ls`, 2 → `mcpwm0`, 3 → `mcpwm1`, on GPIO 2, 4, 5, 13.

## 2. PWM signals (scope or logic analyzer, no servos attached yet)

Measure **one servo per block** (servos 0–3 by default). The MCPWM clock setup changed in ESP-IDF 6, so MCPWM matters most.

- [ ] **Period:** 20.00 ms (50 Hz) on every block.
- [ ] **Pulse widths with `$Servo/Test=n,<deg>`:**

  | Angle | Expected pulse |
  |---|---|
  | 0° | 500 µs |
  | 90° | 1450 µs |
  | 180° | 2400 µs |

  Tolerance is about ±2 µs (resolution is about 0.3 µs on LEDC and MCPWM).
- [ ] **No glitches:** drag the Calibrate slider quickly. There must be no runt or double pulses; changes take effect at period boundaries.
- [ ] **Power-up stagger:** outputs stay low at boot, then switch on one after another, 50 ms apart.
- [ ] **Disabled servo:** after `$/servo/0/enable=false` the line stays low. Set it back to `true`.
- [ ] `$Servo/Release=*` afterwards.
- [ ] **Pulse clamp:**
  1. `$/pwm/ledc_freq_hz=400`, then `$Bye`.
  2. Boot reports `pulse does not fit the PWM period … clamped to 2250 us`.
  3. On the scope, the pulse never fills the whole period.
  4. Restore with `$/pwm/ledc_freq_hz=50`, then `$Bye`.

## 3. PCA9685 (if used)

- [ ] **Wiring:** SDA = GPIO 21, SCL = GPIO 22, address 0x40, V+ from the servo supply.
- [ ] **Detection:** `$I` shows `PCA9685 board 0 at 0x40 found`. The boot log shows `0x40: prescale P, …`; note P.
- [ ] **Clock calibration:**
  1. Measure the period on one channel, e.g. servo 16 on `pca.0.0`.
  2. Set `$/pca/0/osc_hz = 4096 × (P + 1) / measured_period_in_seconds`, then `$CS` and `$Bye`.
  3. Check that 90° gives 1450 µs within about ±5 µs (resolution is about 4.9 µs).
- [ ] **No I2C errors:** no `I2C write failed` lines in the log while the scene runs.
- [ ] **Missing board:** unplug it and reboot. Expect a warning only, with on-chip servos still running.
- [ ] **Disabled board:**
  1. `$/pca/0/enable=false`, `$CS`, `$Bye`.
  2. `$I` lists `servo 16: … board 0, which is disabled`, one line per affected servo.
  3. Re-enable it afterwards.

## 3b. Digital outputs (if used)

- [ ] **GPIO output:** all free GPIOs are servo pins by default, so free one first: `$/servo/15/pin=none`, then `$/out/0/pin=gpio.33`, `enable=true`, `seq=0:on,1000:off`, `$CS`, `$Bye`. The pin is high for 1 s at the start of every scene cycle (check with an LED + resistor or the scope). It is low right after boot and while the output is disabled.
- [ ] **PCF board:** `$/pcf/0/enable=true` (and `type=pcf8575` for 16 bits), `$Bye`. `$I` shows `PCF8574 board 0 at 0x20 found` and an `[OUTPUT:n pcf.0.x pcf0.x]` line per output.
- [ ] **PCF active low:** with an LED from V+ through a resistor to a PCF pin and `invert=true`, the LED lights exactly when the card's lamp is on.
- [ ] **Hold and force:** global Stop freezes the outputs; `$Output/Stop=0` holds one while the others continue; `$Output/Set=0,on` forces it, `$Output/Release=0` returns to the sequence.
- [ ] **Conflicts:** an output on a GPIO already used by a servo, I²C or the button, on GPIO12, or on a disabled/missing PCF board, is reported by `$I` and stays inactive. Two boards on the same I²C address are reported too.
- [ ] **PCF after a warm restart:** switch an `invert=true` output on, then `$Bye`. After the restart it is off (rewritten within about a second), matching the UI.
- [ ] **No I²C errors:** no `pcf857x … I2C write failed` lines in the log while outputs switch.

## 4. One real servo (MG90S)

- [ ] **End stops:** with one servo on servo 0, test 0°, 90° and 180°. The servo must not buzz or strain at the ends. If it does, adjust `min_us` / `max_us` in the Calibrate tab.
- [ ] **Direction:** if it's mirrored, set `invert=true`.
- [ ] **Test-mode speed:** a big slider jump moves the servo smoothly, at no more than `max_dps` (600 °/s).
- [ ] **Persistence:** `$CS`, power-cycle, and the calibration is still there.

## 5. Motion and Stop/Go

- [ ] **Default scene:** the travelling wave plays. Profiles look right: `bell` and `sine` are soft, `const` is abrupt, `trap` changes with `accel_pct`.
- [ ] **BOOT button:** each press toggles Stop/Go exactly once, with no double toggles. Stop holds every servo in place, and Go resumes exactly where it left off.
- [ ] **Serial realtime:** `!`, `~` and `?` work. **Ctrl-X** resets the scene, and servos blend to the start with no jump.
- [ ] **Per-servo Stop/Go:**
  1. Stop one servo; it holds while the others continue.
  2. Go: it blends back smoothly with no jump, and afterwards moves in sync with its neighbours.
- [ ] **Editing while running:** change servo 31's `delay_ms`, or disable it. **No other servo may jump** (review fix #1).
- [ ] **Release after test:** Release blends the servo back into the scene.
- [ ] **Motion task never blocks** (fix #3): send `$$` over serial and press the BOOT button during the output. Motion must stay smooth.
- [ ] **`!` inside a value** (fix #7): `$/servo/0/name=Hi!` stores the name `Hi!` and does not stop the scene.

## 6. Power with all servos

- [ ] **Supply at boot:** stays stable thanks to the stagger. No brownout reset; the log shows no `Brownout detector` / `rst:0xf`.
- [ ] **Supply during the wave:** voltage dip and ripple are acceptable. Add capacitance if servos twitch.
- [ ] **Signal lines:** clean on the scope with all servos moving, with no ground bounce causing jitter.
- [ ] **Stronger servos later:**
  - check their pulse range and adjust `min_us`, `max_us`, `range_deg`, `max_dps`
  - check they accept 3.3 V signals; otherwise add a 74AHCT125 buffer
  - recheck the current budget

## 7. Settings, storage and serial console

- [ ] **Persistence:** change a few settings, `$CS`, power-cycle, and all are back. `$$` shows them.
- [ ] **Factory defaults:** `$RST=*`, then `$Bye`. Defaults are back.
- [ ] **Backup round trip:** Settings → Download config, change something, Restore config, Save. The original values are back.
- [ ] **Clean replies:** log output never appears in the middle of a reply *line*. During a long `$$` reply a log line may appear between two reply lines.

## 8. WiFi and web UI

- [ ] **Joining:** join `ServoScene-XXXX` with the password `servoscene`.
  - [ ] Android shows the captive-portal page by itself.
  - [ ] iPhone shows the captive-portal page by itself.
  - [ ] The laptop reaches http://192.168.4.1/.
- [ ] **Layout:** single column on the phone, grid on the laptop. The header buttons are easy to tap.
- [ ] **Live status:** the status and position dots update about 5 times per second. Two clients at once see the same state, and the BOOT button shows up in both.
- [ ] **Restart flow:** change a pin, then the banner offers "Save & restart", and the UI reconnects after the reboot.
- [ ] **Phone keyboards** (fix #9):
  - `trim_us` accepts `-20`
  - `12,5` is accepted as 12.5
  - the update-server field accepts an IP address
- [ ] **Dead client** (fix #5):
  1. Open the UI on the phone and on the laptop.
  2. Put the phone into airplane mode without closing the page.
  3. The laptop UI stays responsive.
  4. Within about 20–30 s the log shows the phone's WebSocket closing, and `$I` shows one client fewer.

## 9. OTA updates

Before these tests, bump `PROJECT_VER` in `CMakeLists.txt` (e.g. to `0.1.1`) and build, so the update is visible.

- [ ] **Push:**
  1. Settings → Firmware → Upload .bin…, choose `.pio/build/esp32dev/firmware-signed.bin`.
  2. Progress is shown, the scene holds, then the device restarts.
  3. `$I` shows `0.1.1 in ota_1 (not yet confirmed)`.
  4. After 10 s the log shows `new firmware confirmed`.
- [ ] **Pull:**
  1. Bump the version again and run the VS Code task **OTA: serve firmware (port 9999)**.
  2. Check the prefilled address is the laptop's. If the phone joined first it shows the phone's; correct it.
  3. Press "Update from server". Progress `[OTA:download …%]` appears, then a restart with the new version.
- [ ] **Unsigned image is rejected:**
  1. Upload the plain `firmware.bin`. It's rejected with a signature error and the scene resumes.
  2. `$I` no longer shows a version for the other slot (fix #8: header erased).
- [ ] **Foreign key is rejected:** sign with a throwaway key (`scripts/ota_sign_tool.py genkey /tmp/k.pem`, then `sign`). It's rejected.
- [ ] **Non-firmware file is rejected:** a random file, or another project's `.bin`, is rejected before any flash is erased. The UI already refuses files that don't start with byte 0xE9.
- [ ] **Interrupted upload** (fix #2):
  1. Start an upload, then turn off WiFi on the uploading device or close the tab.
  2. Within about 15 s the web server responds again, the scene resumes, and an error is reported.
- [ ] **`https://` is refused:** `$Firmware/Update=https://x/fw.bin` is rejected immediately.
- [ ] **Rollback:**
  1. Build a deliberately broken firmware, e.g. a temporary `abort();` at the end of `app_main`, and install it by OTA.
  2. It crashes and resets before the 10 s confirmation.
  3. The bootloader goes back to the previous version; `$I` shows it.
  4. Remove the `abort()`.
- [ ] **Serial flash after OTA:** `pio run -t upload` still works and boots the serially flashed image.

## 10. Long run

- [ ] **Heap:** `$I` shows `Heap free … largest block … lowest since boot`; the lowest value should stay above about 30 KB.
- [ ] **Stack headroom:** after the long run, `$I` shows `Stack free` for each task. Every value should stay above about 512 bytes; report any task below that.
- [ ] **Stability:** run the full scene for 1–2 hours with the UI open on a phone. No resets, panics or `Guru Meditation` lines in the log. `?` shows the scene time and cycle count rising steadily.
- [ ] **Servo temperature:** check servo temperatures and the supply after the long run.
