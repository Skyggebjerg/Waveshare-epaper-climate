# Waveshare ESP32-S3-ePaper-1.54 — Humidity-First Climate Display

A PlatformIO / Arduino-framework firmware for the Waveshare
[ESP32-S3-ePaper-1.54](https://docs.waveshare.com/ESP32-S3-ePaper-1.54)
board (plain black/white version, hardware revision "V2" — not the
4-color "1.54G" variant). It shows relative humidity as the big
headline reading, with time, date, temperature and battery level around
it, updates with flicker-free partial refreshes, and deep-sleeps
between updates to run for a long time off a small LiPo cell.

**Everything in this README was learned and verified on the real
board** across an extended debugging effort. The two hard-won rules —
how to sleep without breaking the display, and how partial updates are
kept in sync with the layout — are marked clearly below. For the full
debugging history (the RTC library bug, the failed experiments, the
board's audio/SD/PMIC research), see [`NOTES.md`](NOTES.md).

## What it does

- Wakes every 2 minutes (`SLEEP_SECONDS`), on a BOOT-button press, or
  on power-on/reset.
- Reads the onboard SHTC3 sensor (temperature/humidity), the PCF85063
  RTC (time/date), and the battery voltage.
- Draws the display: humidity huge in the center (Orbitron Bold 70)
  with a stacked %/RH label, time and battery in a top status strip,
  temperature and date centered below.
- Refreshes only the screen regions whose values changed — no
  full-screen flash on a normal update. A full cleaning flash runs
  automatically every ~30 minutes (see anti-ghosting below).
- Stays awake `AWAKE_MILLIS` (default 30 s) with the LED on, then
  deep-sleeps. Average draw is dominated by the ESP32-S3's deep-sleep
  current (~20–30 µA) plus ~1 µA for the hibernated panel.
- Sets the RTC from the firmware's build time only when the RTC
  reports lost backup power or an implausible date; a healthy RTC is
  never touched.

## THE display rule: never cut the panel's power during sleep

This cost more debugging time than everything else combined, so it
gets its own section.

The SSD1681 e-paper controller keeps the previously-shown image in
internal RAM, and **partial (non-flashing) refreshes only work by
diffing against that RAM**. This firmware therefore sleeps the panel
with `display.hibernate()` — the controller's own deep-sleep mode
(~1 µA, RAM retained) — and **keeps the panel's power rail ON through
deep sleep** (GPIO6 held LOW by `gpio_hold_en`).

An earlier version cut the panel's power rail every sleep to save that
last ~1 µA. The failure this caused was maddeningly misleading:

- With a **2-minute** sleep, everything looked perfect — the unpowered
  RAM decayed slowly enough to limp through.
- With a **5-minute** sleep, the very next partial update dirtied the
  panel: a grainy band ~5 mm wide around the edges, getting worse each
  wake.

Two attempted workarounds (tight pixel-diff bounding boxes; rewriting
the full frame into controller RAM via `epd2.writeImage` before each
partial refresh) did **not** fix it. Keeping the rail powered fixed it
completely, at a cost of ~1 µA against the ESP32's own ~20–30 µA sleep
draw. If you ever "optimize" GPIO6 back to off-during-sleep, this
whole failure mode returns — and it will look fine in short-interval
testing.

## Partial updates: how they work, and their one maintenance rule

The firmware remembers the last-drawn text of each field (time, date,
temperature, humidity, battery) in RTC memory, which survives deep
sleep. On each wake it compares the fresh values and refreshes only
the fixed screen region(s) of the field(s) that changed, via GxEPD2's
`setPartialWindow` + `display.init(115200, false, 20, false)`
(`initial_refresh=false` — prevents a full-screen flash on every
wake). If nothing changed, the display isn't touched at all.

**The maintenance rule:** the `*_RECT` table in `main.cpp` is a
hand-maintained mirror of the layout. If you move a `*_BASELINE` (or
any drawing position) by more than a few pixels, move its matching
rect too — a field drawn outside its own rect leaves stale pixels
behind on partial updates, which looks like ghosting/doubling.

(A "smarter" full-frame pixel-diff version without this rule was tried
and behaved worse on the real panel; the per-field approach is the one
that runs clean. Don't resurrect the pixel diff without hardware
testing.)

### Anti-ghosting

Partial refreshes leave residue behind over time — a property of the
panel. A full flashing refresh is forced when either limit is hit,
whichever comes first:

- `FULL_REFRESH_AFTER_SECONDS` (default 1800 ≈ 30 min of accumulated
  sleep) — the main, time-based limit, so changing `SLEEP_SECONDS`
  doesn't stretch the cleanup interval;
- `FULL_REFRESH_EVERY` (default 15 partial updates) — a backstop
  against many rapid button-press wakes.

## Configuration knobs

All near the top of `src/main.cpp`:

| Constant | Default | Meaning |
|---|---|---|
| `SLEEP_SECONDS` | 120 | Time between updates (any value is safe for the display now) |
| `AWAKE_MILLIS` | 30000 | How long the display/LED/Serial stay up before sleeping |
| `FULL_REFRESH_AFTER_SECONDS` | 1800 | Max accumulated sleep between cleaning flashes |
| `FULL_REFRESH_EVERY` | 15 | Max partial updates between cleaning flashes |
| `TOP/HERO/TEMP/DATE_BASELINE`, `HERO_LABEL_DROP` | 22/112/162/196, 36 | Vertical text positions (move the matching `*_RECT` for big moves!) |
| `VOLT_FULL` / `VOLT_EMPTY` | 4.12 V / 3.0 V | Battery 100%/0% — Waveshare's own factory thresholds, not the textbook 4.2 V |

Horizontal centering is automatic (measured with `getTextBounds`), so
there are no X positions to maintain.

## Fonts

The display uses three Orbitron fonts in Adafruit GFX format. Their
header files must sit next to `main.cpp` in `src/`:
`Orbitron_Bold_70.h` (humidity number), `Orbitron_Bold_32.h`
(temperature, % sign), `Orbitron_Medium_20.h` (time, date, battery,
labels). The `GFXfont` symbol inside each header must match the
filename. The character set must include digits plus `% : / . -`.

## Getting it running

1. Open this folder in VS Code with the **PlatformIO IDE** extension.
2. Plug the board in over USB-C.
3. Build (checkmark) then Upload (arrow) from the PlatformIO toolbar.
4. Serial Monitor (plug icon) at 115200 baud shows each wake's
   readings and what kind of display update it performed.

**Uploading while the board is asleep**: the native USB port drops out
during deep sleep (the board has no separate USB-serial chip), so the
board disappears and reappears as a USB device every cycle. If an
upload can't find it: hold **BOOT**, tap **RESET**, release BOOT —
that forces USB download mode regardless of sleep state.

**Press BOOT** at any time to wake it for a fresh reading immediately.

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

Two hard-learned warnings:

- **Do not set flash to 16MB** (`board_upload.flash_size` /
  `default_16MB.csv`) — the board has 8 MB. The mismatch crash-loops
  before `setup()` with `spi_flash: Detected size(8192k) smaller than
  the size in the binary image header(16384k)`.
- **Do not switch the RTC library to `lewisxhe/SensorLib`** — its
  `begin()` runs a chip-identity self-test that consistently fails on
  this exact board (`"Device is offline!"`), even though the RTC is
  fine. The Soldered library does a plain register read and works.
  Full story in NOTES.md.

## Hardware reference

| Signal | GPIO | Notes |
|---|---|---|
| e-Paper CS / DC / RST / BUSY / SCK / MOSI | 11 / 10 / 9 / 8 / 12 / 13 | plain default `SPI` object, `SPI.begin(sck, -1, mosi, cs)` |
| e-Paper panel power switch | 6 | **active LOW** (LOW = on). Held LOW through deep sleep — see THE display rule above. Note: generic AI/internet examples often claim HIGH = on for this pin; Waveshare's own source and this hardware say LOW |
| Battery power latch | 17 | must be driven HIGH early in boot or the board switches itself off; held HIGH across deep sleep |
| Onboard LED #1 | 3 | **active LOW** (LOW = on); lit while awake |
| Battery voltage sense (ADC1 ch3) | 4 | 200k/200k divider — readings ×2 |
| BOOT button | 0 | deep-sleep wake source (`ext1`, active low) |
| PWR button | 18 | not used by this firmware |
| PCF85063 alarm/INT | 5 | not used; usable as an extra `ext1` wake source during deep sleep only (it is NOT wired to turn a fully powered-off board back on — see NOTES.md) |
| I2C SDA / SCL | 47 / 48 | shared: SHTC3 (0x70), PCF85063 (0x51), and the ES8311 audio codec (0x18) if audio is ever added |

Chip: ESP32-S3-PICO-1-N8R8 (8 MB flash, 8 MB PSRAM). Panel: 200×200
SSD1681 (`GxEPD2_154_D67`).

RTC quirk: the Soldered library stores years as an offset from 1970,
so the plausible-year check in the firmware accepts 2024–2069.

## Known limitations

- **Battery charging status is not exposed to firmware** — confirmed
  absent from Waveshare's entire software repo and docs; the PMIC
  (ETA6098) is a standalone charge-management IC.
- **A second onboard LED** exists with no software control — likely a
  hardware-only charge/power indicator.
- **Spare GPIOs**: essentially every usable pin is claimed by the
  display, sensors, buttons, SD, and audio (pin maps for the unused SD
  card and ES8311 mic/speaker are in NOTES.md). GPIO 1, 2, 7, 21, 43,
  44 appear in no official example, but whether they're broken out is
  unconfirmed without a schematic.

## Project files

- `src/main.cpp` — the firmware, single source of truth.
- `src/Orbitron_*.h` — the three display fonts (user-provided).
- `platformio.ini` — build configuration.
- `NOTES.md` — debugging history and hardware research: the RTC
  library bug, the display/power saga in full, lessons learned,
  audio/SD/PMIC/GPIO findings.

## References

- Waveshare's official example repo:
  https://github.com/waveshareteam/ESP32-S3-ePaper-1.54
- Independent working Arduino reference for this board:
  https://github.com/VolosR/waveshareEinkMonitor
- Soldered PCF85063A RTC library:
  https://github.com/SolderedElectronics/Soldered-PCF85063A-RTC-Module-Arduino-Library
- GxEPD2: https://github.com/ZinggJM/GxEPD2