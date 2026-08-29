# Waveshare ESP32-S3-ePaper-1.54 — Battery Climate Display

A PlatformIO / Arduino-framework firmware for the Waveshare
[ESP32-S3-ePaper-1.54](https://docs.waveshare.com/ESP32-S3-ePaper-1.54)
board (plain black/white version, hardware revision "V2" — not the
4-color "1.54G" variant). It reads the onboard temperature/humidity
sensor and real-time clock, shows them on the e-paper display alongside
the time and battery level, and deep-sleeps between updates to run for
a long time off a small LiPo cell.

For the full story of how this firmware was debugged into existence
(including a nasty RTC library bug and its fix, and research into the
board's other hardware), see [`NOTES.md`](NOTES.md) in this same folder.
This README is the "what it does and how to build it" reference; NOTES.md
is the "why it's built this way" deep-dive.

## What you need

- The Waveshare ESP32-S3-ePaper-1.54 board
- A USB-C cable
- [Visual Studio Code](https://code.visualstudio.com/) with the
  [PlatformIO IDE extension](https://platformio.org/platformio-ide) installed

## What it does

- Wakes every 2 minutes (or immediately on a BOOT-button press, or on
  power-on/reset).
- Reads the onboard SHTC3 temperature/humidity sensor and the PCF85063
  real-time clock.
- Reads the battery voltage and estimates a charge percentage.
- Draws time, date, temperature, humidity, and battery level on the
  200x200 e-paper display.
- Refreshes only the part of the screen that actually changed since the
  last wake, instead of a full-screen flash every time — **confirmed
  working on real hardware**, see "Partial updates" below.
- Stays awake for 30 seconds after drawing (so the reading and Serial
  output can be observed), then deep-sleeps for 2 minutes.
- Lights the onboard LED for the whole time it's awake, as a visual
  "still running" indicator.
- Sets the RTC's clock from the firmware's build time, but only as a
  fallback — if the RTC has backup power and a plausible date, its own
  time is trusted and left alone.

## Getting it running

1. Open this folder in VS Code (`File -> Open Folder...`).
2. Wait for the PlatformIO icon (an alien head) to appear in the left
   sidebar — it'll index the project automatically.
3. Plug the board in over USB-C.
4. In the blue status bar at the bottom of VS Code, click the checkmark
   icon to **Build**, then the right-arrow icon to **Upload**. PlatformIO
   will download the ESP32 toolchain and all required libraries the first
   time — that can take a few minutes.
5. Click the plug icon to open the **Serial Monitor** (115200 baud) and
   watch it print the reading it just took, right before it goes to sleep.

If the upload can't find the board, or it fails partway through: hold the
**BOOT** button, tap **RESET** (or plug the cable in while still holding
BOOT), then release BOOT and try Upload again. This forces the ESP32-S3
into its USB download mode — needed because every deep-sleep cycle drops
the USB connection entirely (see "Usage notes" below), so it may not be
sitting at a normal, uploadable boot when you plug in.

### `platformio.ini`

```ini
[env:waveshare-esp32s3-epaper154]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino

; --- Board has an ESP32-S3-PICO-1-N8R8: 8 MB flash ---
board_upload.flash_size = 8MB
board_build.partitions = default.csv

build_flags =
    -D ARDUINO_USB_CDC_ON_BOOT=1
    -D ARDUINO_USB_MODE=1

monitor_speed = 115200
upload_speed = 921600

lib_deps =
    zinggjm/GxEPD2 @ ^1.6.9
    adafruit/Adafruit GFX Library @ ^1.12.6
    adafruit/Adafruit BusIO @ ^1.17.4
    adafruit/Adafruit Unified Sensor @ ^1.1.15
    adafruit/Adafruit SHTC3 Library @ ^1.0.2
    https://github.com/SolderedElectronics/Soldered-PCF85063A-RTC-Module-Arduino-Library.git
```

All libraries are pulled in automatically — no manual installation
needed. **Do not** set `board_upload.flash_size` to 16MB or use a
`default_16MB.csv` partition table — this board only has 8MB of flash.
That mismatch crash-loops the board before `setup()` ever runs. See
NOTES.md if you ever hit this.

## How it works

Everything happens once per wake, inside `setup()`:

1. **Re-latch board power.** This board's battery path only stays on if
   firmware drives GPIO17 high — that's how the physical power button
   keeps the board alive after you let go of it. The very first thing
   the sketch does is set that pin (and GPIO6, the e-paper panel's power
   switch, and the onboard LED), releasing any "hold" left over from the
   last deep sleep.
2. **Read the PCF85063 RTC** over I2C for the current date/time. It runs
   on its own backup power across sleep cycles, so its clock isn't reset
   every wake — it's only set from the firmware's build time if the chip
   reports it lost backup power, or shows an implausible year.
3. **Read the SHTC3 sensor** over the same I2C bus for temperature and
   humidity.
4. **Read the battery voltage** via ADC and estimate a charge percentage.
5. **Decide what changed** since the last wake (see "Partial updates"
   below), and draw to the e-paper over SPI using
   [GxEPD2](https://github.com/ZinggJM/GxEPD2) — refreshing only the
   changed region where possible.
6. **Stay awake 30 seconds**, then **go to deep sleep.** The e-paper
   controller is put into its own low-power mode, its power rail is cut,
   both power-latch pins are "held" so they survive the ESP32's
   power-domain shutdown, and `esp_sleep_enable_timer_wakeup()` plus a
   BOOT-button `ext1` wake schedule the next wake-up.

## Usage notes

- **USB drops out during deep sleep.** The native USB port loses power
  along with everything else and only comes back once the chip wakes and
  re-boots. This board only has a native USB-C port (no separate
  USB-serial chip), so it will briefly disappear and reappear as a USB
  device every cycle — normal, and only visible while a computer is
  plugged in.
- **To force an immediate refresh** instead of waiting out the 2-minute
  interval, just press BOOT — it's wired as a deep-sleep wake source.

## Configuration

The knobs you're most likely to want to change are all `static const` /
`const` values near the top of `src/main.cpp` or in the relevant
function:

| Constant | Default | Meaning |
|---|---|---|
| `SLEEP_SECONDS` | 120 | Time between updates |
| `VOLT_FULL` / `VOLT_EMPTY` (in `batteryPercentFromVoltage`) | 4.12V / 3.0V | Battery 100%/0% thresholds — matches Waveshare's own factory firmware, not the textbook 4.2V |
| the `delay(30000)` in `setup()` | 30s | How long the display/LED/Serial stay on before sleeping again |

## Partial updates (value remembering)

To avoid a full-screen flash on every wake, the firmware remembers what
it last drew — in RTC memory, which survives deep sleep — and compares
the newly-read time/date/temperature/humidity/battery text against that
memory. Only the on-screen regions that actually changed get refreshed;
if nothing changed at all, the display isn't touched that cycle.

Each of the five fields (time, date, temperature, humidity, battery) is
checked independently and owns a fixed region of the screen. Only the
regions belonging to fields that changed get unioned into that wake's
refresh window — so if only the minutes ticked over, just the time
region updates; if temperature also drifted a little, that region gets
included too, while humidity and battery stay untouched if they didn't
change.

This relies on GxEPD2's `initial_refresh=false` init option, which
normally assumes the e-paper panel's own power was never cut — only the
MCU slept. This firmware cuts the panel's power on every single cycle
instead (GPIO6, for battery life), which in theory also wipes the SSD1681
controller's own internal comparison memory that partial updates are
diffed against. Despite that, **this has been confirmed working well on
real hardware** — exactly why isn't fully explained from the datasheet
alone, so treat it as an empirically-verified behavior on this specific
board/library combination rather than a guaranteed-by-design one. Partial
refreshes are also known to accumulate slight ghosting over many cycles
(a property of the panel, not this code) — if that becomes visible after
extended use, forcing an occasional full refresh (e.g. every N cycles)
would clear it; not currently implemented.

## Hardware reference

| Signal | GPIO | Notes |
|---|---|---|
| e-Paper CS / DC / RST / BUSY / SCK / MOSI | 11 / 10 / 9 / 8 / 12 / 13 | plain default `SPI` object, not a separate `SPIClass` instance |
| e-Paper panel power switch | 6 | active LOW (LOW = panel on) |
| Battery power latch | 17 | must be driven HIGH early in boot or the board switches itself off; held across deep sleep |
| Onboard LED #1 | 3 | active LOW (LOW = on); used here as an "awake" indicator |
| Battery voltage sense (ADC1 ch3) | 4 | behind a 200k/200k divider — readings need ×2 |
| BOOT button | 0 | wired up as a deep-sleep wake source |
| PWR button | 18 | not currently used by this firmware |
| PCF85063 alarm/interrupt output | 5 | not currently used by this firmware (see NOTES.md for why this isn't a free "fully powered off" wake source) |
| I2C SDA / SCL (SHTC3, PCF85063) | 47 / 48 | also shared by the ES8311 audio codec (`0x18`) if audio is ever added |

Chip: ESP32-S3-PICO-1-N8R8 (8 MB flash, 8 MB PSRAM, dual-core, Wi-Fi +
BLE). e-Paper: 200x200, SSD1681 controller (Good Display GDEH0154D67
panel).

Onboard LED #2 exists but has no known software control — likely a
hardware-only charge/power indicator (see "Known limitations" below).
Battery charging status is not exposed to firmware at all on this board.

This pin map, and the GPIO6/GPIO17 power-latch behavior in particular,
come from Waveshare's own example firmware for this exact board,
published at
[github.com/waveshareteam/ESP32-S3-ePaper-1.54](https://github.com/waveshareteam/ESP32-S3-ePaper-1.54).
This project reimplements the same behavior as a plain Arduino-framework
PlatformIO sketch — no LVGL, no ESP-IDF, no GUI-Guider generated UI — so
it's easier to read, build on, and maintain from VS Code.

## Libraries used

| Library | Purpose |
|---|---|
| [GxEPD2](https://github.com/ZinggJM/GxEPD2) | Drives the SSD1681 e-paper controller |
| [Adafruit GFX Library](https://github.com/adafruit/Adafruit-GFX-Library) | Text/graphics primitives (a GxEPD2 dependency) |
| [Adafruit SHTC3 Library](https://github.com/adafruit/Adafruit_SHTC3) | Reads the onboard temperature/humidity sensor |
| [Soldered PCF85063A](https://github.com/SolderedElectronics/Soldered-PCF85063A-RTC-Module-Arduino-Library) | Drives the onboard PCF85063 real-time clock |

All are pulled in automatically by `platformio.ini` — no manual library
installation needed. (An earlier version of this project used
`lewisxhe/SensorLib` for the RTC instead; it had a self-test that
consistently failed on this exact board — see NOTES.md for the full
story of that bug and its fix.)

## Known limitations / open questions

- **Battery charging status is not exposed to firmware.** Confirmed
  absent from Waveshare's entire official software repo and their
  documentation — the onboard PMIC (marked ETA6098) has no
  firmware-visible interface.
- **A second onboard LED exists with no known software control.** Likely
  a hardware-only charge/power-present indicator; unconfirmed without a
  schematic.
- **Spare GPIOs are unclear.** Between the display, sensors, buttons,
  and battery sensing, most commonly-usable pins are already claimed. A
  handful (GPIO1, 2, 7, 21, 43, 44) appear in no official example, but
  whether any are broken out to an accessible header pad on this board
  is unconfirmed.
- **No compiler/hardware access during development.** This firmware was
  built and debugged entirely by reading library/vendor source and
  reasoning from real serial output provided during development — not by
  local compilation. See NOTES.md for the full story, including one real
  bug this caused and how it was found. Flag anything that doesn't build
  or behave as expected and it can be fixed from there.

## Other onboard hardware (researched, not used by this firmware)

The board also has a microphone + speaker (single ES8311 codec) and a
microSD card slot, neither of which this firmware touches. Pin maps and
API notes for both are in NOTES.md, under "Other onboard peripherals",
sourced from Waveshare's official Arduino examples
(`08_Audio_Test`, `04_SD_Card`).

Waveshare also ships LVGL 8 and 9 as vendorable libraries
(`01_Arduino_Libraries/lvgl8`/`lvgl9` in their repo) for a fancier UI, but
their example driving this exact display does so through a completely
separate raw SPI driver, not GxEPD2 — adopting it as-is would mean
replacing the display stack this firmware already has working, not
adding to it. See NOTES.md for the full reasoning.

## Project files

- `src/main.cpp` — the firmware (always the single source of truth; this
  is the file to flash).
- `platformio.ini` — build configuration (see above).
- `NOTES.md` — the deep debugging history: the RTC library bug and fix,
  lessons learned, and hardware research (audio/SD/PMIC/GPIOs) not
  covered in this README.

## References

- Waveshare's official example repo:
  https://github.com/waveshareteam/ESP32-S3-ePaper-1.54
- Independent working Arduino reference for this exact board (the sketch
  that broke an early I2C debugging deadlock):
  https://github.com/VolosR/waveshareEinkMonitor
- Soldered PCF85063A Arduino library:
  https://github.com/SolderedElectronics/Soldered-PCF85063A-RTC-Module-Arduino-Library
