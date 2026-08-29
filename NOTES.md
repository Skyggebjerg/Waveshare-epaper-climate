# Waveshare ESP32-S3-ePaper-1.54 — PlatformIO & sensor notes

Working knowledge collected from a live debugging session on the actual
board. Board: https://docs.waveshare.com/ESP32-S3-ePaper-1.54 (plain
black/white version, hardware revision "V2" — not the 4-color "1.54G"
variant).

## Hardware summary

- Chip: ESP32-S3-PICO-1-N8R8 — **8 MB flash**, 8 MB PSRAM, Wi-Fi + BLE
- Display: 1.54" 200x200 monochrome e-paper, SSD1681 controller
  (`GxEPD2_154_D67` in GxEPD2)
- Onboard temperature/humidity sensor: SHTC3, I2C address `0x70`
- Onboard RTC: PCF85063, I2C address `0x51`, battery-backed (survives
  deep sleep and firmware re-flashes)
- I2C bus (shared by both of the above): SDA = GPIO47, SCL = GPIO48
- e-Paper SPI pins: CS=11, DC=10, RST=9, BUSY=8, SCK=12, MOSI=13
- e-Paper panel power switch: GPIO6, **active LOW** (LOW = panel on)
- Battery power latch: GPIO17 — firmware must drive this **HIGH** early in
  boot or the board switches itself off shortly after the power button is
  released. Freeze it with `rtc_gpio_hold_en()`/`gpio_hold_en()` before
  deep sleep, release with the `_dis()` counterpart on every boot before
  driving it again.
- Onboard LED #1: GPIO3 — **active LOW** (LOW = on), confirmed on real
  hardware (the initial HIGH=on assumption, copied from a reference sketch,
  turned out to be wrong). Used in this project as an "awake" indicator:
  on for the whole time between power-latch and going to sleep.
- Onboard LED #2: exists on the board but has **zero software references**
  anywhere in Waveshare's example repo. Likely a hardware-only
  charge-status or power-present indicator wired directly to the charging
  IC, not a GPIO-controlled LED. Not confirmed without a schematic.
- Battery voltage sense: GPIO4 = ADC1_CHANNEL_3, behind a 200k/200k
  divider (readings need x2). Confirmed by three independent sources:
  Waveshare's `01_ADC_Test` Arduino example, the factory ESP-IDF firmware's
  `port_adc.cpp`, and an independent hobbyist reference sketch. Percent
  mapped linearly between 3.0V (empty) and 4.12V (full) — Waveshare's own
  factory-firmware thresholds, not the textbook 4.2V, since this board's
  charge circuit tops out lower.
- Battery **charging status is not exposed to firmware at all** — grepped
  the entire Waveshare repo (ESP-IDF + Arduino, all examples) for
  charge/CHG/PMIC/ETA6098/stat_pin/vbus/pgood terms with zero hits, and
  their docs/wiki only mention a generic "lithium battery recharge
  management circuit." The PMIC (marked ETA6098 on the board) appears to
  be a standalone charge-management IC with no firmware-visible interface.
- BOOT button: GPIO0. PWR button: GPIO18. RTC alarm/interrupt pin: GPIO5
  (PCF85063's INT output — see "Deep sleep vs. real power-off" below).

## Known-good `platformio.ini`

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

**Do not** set `board_upload.flash_size` to 16MB or use a `default_16MB.csv`
partition table — this board only has 8MB of flash. Doing so builds a
binary the bootloader can never load; it crash-loops before `setup()` ever
runs, with the telltale line:

```
E (90) spi_flash: Detected size(8192k) smaller than the size in the binary image header(16384k). Probe failed.
assert failed: do_core_init startup.c:328 (flash_ret == ESP_OK)
```

If you ever see that exact error, it's this — not an application bug.
Fix by matching `board_upload.flash_size`/`board_build.partitions` back to
8MB/`default.csv`, then Clean + rebuild + reupload (a stale `.pio/build`
can otherwise leave the old 16MB image in place, so confirm the ELF/SHA
actually changes after reflashing).

## How to actually get sensor data (the part that took the longest)

The working recipe, in order:

1. **Power-up sequence in `setup()`, before touching I2C at all:**
   - Release any GPIO holds left over from a previous deep sleep
     (`gpio_hold_dis()` / `rtc_gpio_hold_dis()` on GPIO17 and GPIO6).
   - Drive GPIO17 (`VBAT_PWR`) **HIGH**.
   - Drive GPIO6 (`EPD_PWR`) **LOW** (panel on).
   - Drive GPIO3 (onboard LED) **LOW** (on — confirmed active-low on real
     hardware, see "Unexplained but proven" below).
   - A short delay (10ms is enough in practice) before touching I2C.
2. **Call `Wire.begin(47, 48)` yourself, explicitly**, before constructing
   or calling `begin()` on any sensor/RTC library. Don't rely on a library
   to configure the bus indirectly — one of the earlier bugs here came
   from doing exactly that.
3. **SHTC3**: `Adafruit_SHTC3` (the standard Adafruit library) works fine
   on this hardware over plain `Wire`. No workaround needed here.
   - Caveat: `Adafruit_SHTC3::getEvent()` retries forever with **no
     timeout** if the sensor never answers. Only call it after `begin()`
     has returned `true` — otherwise a missing/unresponsive sensor hangs
     the firmware forever.
4. **PCF85063 RTC**: use the **Soldered PCF85063A** library
   (`SolderedElectronics/Soldered-PCF85063A-RTC-Module-Arduino-Library`,
   header `PCF85063A-SOLDERED.h`), **not** `lewisxhe/SensorLib`.
   - `lewisxhe/SensorLib`'s `SensorPCF85063::begin()` runs a "is this
     really a PCF85063 and not a pcf8563" self-test: it writes a RAM
     register, reads it back, and restores it — several extra I2C
     transactions beyond just reading the time. On this board, that
     self-test consistently failed and logged `"Device is offline!"`,
     even though the underlying bus may have been perfectly capable of a
     plain read.
   - The Soldered library's `begin()` just calls `Wire.begin()` (a no-op
     if you already called it yourself) and its getters
     (`getHour()`/`getMinute()`/etc.) do a plain register read with no
     self-test. That's what actually worked.
5. **e-Paper SPI**: use the plain default `SPI` object, not a separate
   `SPIClass(HSPI)`/`SPIClass(FSPI)` instance:
   ```cpp
   SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS); // -1 = no MISO needed
   display.epd2.selectSPI(SPI, SPISettings(4000000, MSBFIRST, SPI_MODE0));
   display.init(115200);
   ```
   A separate `SPIClass(HSPI)` instance also works but complains
   (`spiAttachMISO(): HSPI Does not have default pins on ESP32S3!`) if you
   pass `-1` for MISO on it, and needs a dummy real GPIO handed to it
   instead. The default `SPI` object accepts `-1` cleanly.

### Unexplained but proven

An independent, working Arduino sketch for this exact board
(github.com/VolosR/waveshareEinkMonitor) drives GPIO3 (the onboard LED)
in the same breath as the two power-latch pins, every boot, before
touching I2C. There's no confirmed hardware reason this should matter for
sensor communication — but it costs nothing to include, a fellow builder
converged on it independently, and it's in the version that finally
worked. Left in as a "why not" until/unless it's proven irrelevant.

Separately (and now confirmed on real hardware, unrelated to the sensor
question): GPIO3 is **active-low** — driving it HIGH (the polarity the
reference sketch and an early version of this project's firmware both
used) did not turn the LED on. Driving it LOW does. This project now uses
GPIO3 intentionally as an "awake" indicator (LOW/on for the whole time
between the power latch and going to sleep, HIGH/off right before
`esp_deep_sleep_start()`), not just carried over unexplained.

## Lessons learned (so the next debugging session doesn't repeat these)

- **A library's own internal self-test can fail even when the underlying
  bus works.** `SensorPCF85063::begin()`'s "verify chip identity" dance
  was the actual blocker for a long time — not the raw I2C bus, and not
  the hardware. Prefer libraries with the least amount of magic on
  `begin()` when a chip is proving stubborn.
- **An I2C bus recovery routine (bit-banging clocks to free a stuck SDA
  line) can succeed at freeing the bus while the real problem is still
  unsolved.** Don't mistake "SDA/SCL both read HIGH after recovery" for
  "communication now works" — they're different things.
- **A working, independent third-party reference for the exact same board
  is far more valuable than reasoning from a vendor's official firmware**,
  if that firmware is written against a different stack (Waveshare's own
  factory firmware is ESP-IDF using the modern `i2c_master` driver, not
  Arduino's `Wire` — not a line-by-line comparable reference for an
  Arduino/PlatformIO project).
- **This project's build could not be live-compiled in the assistant's
  sandbox** (`registry.platformio.org` returns 403 there). Every fix
  before this one was verified by reading library source, not by
  compiling — real hardware nuance (exact transaction patterns, a
  library's extra validation steps) doesn't always show up in source
  reading alone, which is why this took several iterative rounds against
  real serial output.
- A GCC **internal compiler error: Segmentation fault** during a
  PlatformIO build is toolchain/environment flakiness, not a code bug —
  retry once, then Clean + rebuild, then as a last resort delete and let
  PlatformIO redownload `~/.platformio/packages/toolchain-xtensa-esp32s3`.
- `esp_sleep_enable_ext1_wakeup_io()` doesn't exist in this installed
  arduino-esp32 core version — use `esp_sleep_enable_ext1_wakeup()`
  (same arguments) instead.

## Current firmware status (as of this note)

Confirmed working end-to-end, on real hardware, as a battery-friendly
climate display:

- Wakes on a 2-minute timer, on a BOOT-button press, or on power-on/reset.
- Reads SHTC3 (temp/humidity), PCF85063 (time), and the battery ADC.
- Sets the RTC's clock from firmware compile time, but only as a fallback
  — triggered when a raw read of the PCF85063's seconds register shows the
  oscillator-stopped flag set (lost backup power), or when the read-back
  year is outside a plausible 2024–2069 range. A healthy RTC keeps
  whatever time it already has.
- Draws time/date/temp/humidity/battery percentage on the e-paper display.
- Stays awake 30 seconds after drawing (so the display/Serial can be
  observed), then deep-sleeps for 2 minutes.
- GPIO3 LED is on for the whole awake window as a visual "still awake"
  indicator (active-low — see hardware summary above).
- GPIO17/GPIO6 (VBAT/EPD power latches) are held across deep sleep with
  `gpio_hold_en()`/`gpio_deep_sleep_hold_en()`, released on the next boot
  before being driven again.

## Deep sleep vs. real power-off — an important distinction

Waveshare's own `11_RTC_Sleep_Test` Arduino example (not currently used by
this project, but read in full while researching the board's other
features) reveals two genuinely different low-power states on this board,
and it's easy to conflate them:

1. **ESP32 deep sleep** (what this project uses): `esp_deep_sleep_start()`
   with GPIO17 (VBAT latch) held HIGH throughout via `rtc_gpio_hold_en()`.
   The board's power rail never drops — the ESP32 itself just goes into a
   low-power RTC-domain-only state, still drawing deep-sleep-level
   current. Their example configures **three** `ext1` wake sources this
   way: BOOT (GPIO0), the PWR button (GPIO18), and — new information —
   **the PCF85063's alarm/interrupt pin (GPIO5)**. That third one could be
   added to this project as an alternative/additional wake trigger (wake
   on an RTC alarm register instead of only a fixed timer interval), but
   it is still deep sleep under the hood, not a lower-power state than
   what's already implemented.
2. **Real hardware power-off**: the same example's `get_wakeup_gpio()`
   checks whether a deep-sleep wake was specifically caused by the PWR
   button (GPIO18); if so, it releases the GPIO17 hold and drives
   `VBAT_POWER_OFF()` — actually collapsing the board's 3.3V rail, ESP32
   included. This is a genuine "off," not deep sleep.

**What this does *not* support, as far as the source shows:** a board that
is fully powered off (rail cut, ESP32 unpowered) later turning itself back
on purely because the RTC alarm fired, with no ESP32 involvement. The
GPIO5 "RTC wake" path only works as an `ext1` source *during* ESP32 deep
sleep, where the ESP32 (and the latched rail) are already alive — it is
not wired into the power-off/power-on latch circuit itself in any code
reviewed. Confirming whether the PCF85063's INT pin is *also* hard-wired
into that physical power latch (independent of the ESP32) would need an
actual schematic, which wasn't available.

## Other onboard peripherals (researched, not yet used by this project)

Source: https://github.com/waveshareteam/ESP32-S3-ePaper-1.54/tree/main/02_Example/Arduino
(`04_SD_Card`, `08_Audio_Test`, `11_RTC_Sleep_Test`).

**Microphone + speaker — single ES8311 codec, "in_out" mode** (confirmed
in `08_Audio_Test/src/codec_board/board_cfg.h`, board profile
`S3_ePaper_1_54` — note this is *not* in the more commonly-found generic
`board_cfg.txt`, which only lists unrelated reference boards):

| Signal | GPIO |
|---|---|
| I2S MCLK | 14 |
| I2S BCLK | 15 |
| I2S WS/LRCLK | 38 |
| I2S DIN (mic in) | 16 |
| I2S DOUT (speaker out) | 45 |
| PA (speaker amp) enable | 46 |
| I2C SDA/SCL (codec control) | 47 / 48 — same bus as SHTC3 (0x70) and RTC (0x51) |

The ES8311 sits at its default I2C address (0x18) on the shared bus — no
address conflict with the existing sensor/RTC. There's also a dedicated
power gate for the whole audio subsystem, `Audio_PWR_PIN = GPIO42`,
**active LOW** (same polarity as `EPD_PWR_PIN`) — confirmed in
`board_power_bsp.cpp`'s `POWEER_Audio_ON()`/`POWEER_Audio_OFF()`. There is
only one codec (no separate mic codec like the ES7210 some other
Espressif reference boards use) — one chip does both directions.

**Micro-SD card** — SDMMC 1-line mode, not SPI (confirmed in
`04_SD_Card/sdcard_bsp.h`/`.cpp`):

| Signal | GPIO |
|---|---|
| CLK | 39 |
| CMD | 41 |
| D0 | 40 |

Mounted via `esp_vfs_fat_sdmmc_mount()` at `/sdcard` in the official
example. No separate power-gate pin found for the card.

**Spare GPIOs**: between the display (8 pins), sensors/RTC/battery (I2C +
GPIO3/4/5), both buttons, the SD card, and the audio block, essentially
every commonly-usable pin on this module is already accounted for.
GPIO19/20 are the native-USB D-/D+ lines for the board's USB-C port, so
those aren't free either. A handful of pins (1, 2, 7, 21, 43, 44) don't
appear in any official example — but there's no confirmation from source
alone whether any of those are actually broken out to an accessible
pad/header on this board; that would need the schematic or a look at the
board's silkscreen.

## Reference material

- Waveshare's official example repo:
  https://github.com/waveshareteam/ESP32-S3-ePaper-1.54 — good for the pin
  map and power-latch behavior, but its examples are ESP-IDF (modern
  `i2c_master` driver), not Arduino `Wire` — not a drop-in reference for
  how this project talks to I2C.
- Independent working Arduino reference for this exact board:
  https://github.com/VolosR/waveshareEinkMonitor — the sketch that broke
  the debugging deadlock; see "How to actually get sensor data" above.
- Soldered PCF85063A Arduino library:
  https://github.com/SolderedElectronics/Soldered-PCF85063A-RTC-Module-Arduino-Library
- Audio codec pin profile: `02_Example/Arduino/08_Audio_Test/src/codec_board/board_cfg.h`
  (look for the `S3_ePaper_1_54` entry — the generic `board_cfg.txt` in the
  FactoryProgram folder does NOT contain it).
- SD card pins/mount: `02_Example/Arduino/04_SD_Card/sdcard_bsp.h`/`.cpp`.
- Deep-sleep vs. real power-off reference: `02_Example/Arduino/11_RTC_Sleep_Test/src/power/board_power_bsp.cpp`
  and `user_app.cpp`.
