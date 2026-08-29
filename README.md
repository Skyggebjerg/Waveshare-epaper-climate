# Waveshare ESP32-S3-ePaper-1.54 — Room Climate Display

A PlatformIO project for the [Waveshare ESP32-S3-ePaper-1.54](https://docs.waveshare.com/ESP32-S3-ePaper-1.54).
It reads the board's built-in SHTC3 temperature/humidity sensor, shows the
reading and the current time on the 1.54" e-paper screen, then deep-sleeps
for 2 minutes using the onboard PCF85063 real-time clock to keep the
displayed time correct across sleep cycles (the ESP32-S3's own timer is what
actually wakes the chip up again).

## What you need

- The Waveshare ESP32-S3-ePaper-1.54 board
- A USB-C cable
- [Visual Studio Code](https://code.visualstudio.com/) with the
  [PlatformIO IDE extension](https://platformio.org/platformio-ide) installed

## Getting it running

1. Open this folder in VS Code (`File -> Open Folder...`).
2. Wait for the PlatformIO icon (an alien head) to appear in the left sidebar
   — it'll index the project automatically.
3. Plug the board in over USB-C.
4. In the blue status bar at the bottom of VS Code, click the checkmark icon
   to **Build**, then the right-arrow icon to **Upload**. PlatformIO will
   download the ESP32 toolchain and all required libraries the first time —
   that can take a few minutes.
5. Click the plug icon to open the **Serial Monitor** and watch it print the
   reading it just took, right before it goes to sleep.

If the upload can't find the board, or it fails partway through: hold the
**BOOT** button, tap **RESET** (or plug the cable in while still holding
BOOT), then release BOOT and try Upload again. This forces the ESP32-S3 into
its USB download mode. Also note this board only has a native USB-C port (no
separate USB-serial chip), so every deep-sleep/wake cycle makes it briefly
disappear and reappear as a USB device — normal, and only visible while a
computer is plugged in.

## How it works

Everything happens once per wake, inside `setup()`:

1. **Re-latch board power.** This board's battery path only stays on if
   firmware drives GPIO17 high — that's how the physical power button keeps
   the board alive after you let go of it. The very first thing the sketch
   does is set that pin, and release the "hold" that was placed on it before
   the last deep sleep.
2. **Read the PCF85063 RTC** over I2C for the current date/time. It runs on
   its own backup power across sleep cycles, so its clock isn't reset every
   time the ESP32 wakes — it's only set from the compile-time timestamp if
   the chip reports it lost time.
3. **Read the SHTC3 sensor** over the same I2C bus for temperature and
   humidity.
4. **Draw to the e-paper** over SPI, using [GxEPD2](https://github.com/ZinggJM/GxEPD2).
5. **Go to deep sleep.** The e-paper controller is put into its own
   low-power mode, the panel's power rail is cut, the power-latch pin is
   "held" so it survives the ESP32's power-domain shutdown, and
   `esp_sleep_enable_timer_wakeup()` schedules a wake-up in 2 minutes.

### Changing the update interval

Edit this line near the top of `src/main.cpp`:

```cpp
static const uint64_t SLEEP_SECONDS = 120; // 2 minutes between readings
```

### If the clock ever looks wrong

The RTC only gets (re-)set automatically when it reports it lost its backup
power. If you ever need to force it to a specific time, temporarily replace
the body of `initRtc()` with an unconditional
`rtc.setDateTime(RTC_DateTime(__DATE__, __TIME__));`, upload once, then put
the original conditional logic back and upload again — otherwise it would
re-apply the firmware's build time on every single wake.

## Hardware reference (Waveshare ESP32-S3-ePaper-1.54)

| Signal | GPIO |
|---|---|
| e-Paper CS | 11 |
| e-Paper DC | 10 |
| e-Paper RST | 9 |
| e-Paper BUSY | 8 |
| e-Paper SCK | 12 |
| e-Paper MOSI | 13 |
| e-Paper panel power switch (active low) | 6 |
| Battery power latch (must stay high) | 17 |
| I2C SDA (SHTC3 + PCF85063) | 47 |
| I2C SCL (SHTC3 + PCF85063) | 48 |

Chip: ESP32-S3-PICO-1-N8R8 (8 MB flash, 8 MB PSRAM, dual-core, Wi-Fi + BLE).
e-Paper: 200x200, SSD1681 controller (Good Display GDEH0154D67 panel) — this
is the plain black/white version of the board, not the 4-color "1.54G".

This pin map, and the GPIO6/GPIO17 power-latch behavior in particular, come
from Waveshare's own example firmware for this exact board, published at
[github.com/waveshareteam/ESP32-S3-ePaper-1.54](https://github.com/waveshareteam/ESP32-S3-ePaper-1.54)
(see `02_Example/Arduino/11_RTC_Sleep_Test`). This project reimplements the
same behavior as a plain Arduino-framework PlatformIO sketch — no LVGL, no
ESP-IDF, no GUI-Guider generated UI — so it's easier to read, build on, and
maintain from VS Code.

## Libraries used

| Library | Purpose |
|---|---|
| [GxEPD2](https://github.com/ZinggJM/GxEPD2) | Drives the SSD1681 e-paper controller |
| [Adafruit GFX Library](https://github.com/adafruit/Adafruit-GFX-Library) | Text/graphics primitives (a GxEPD2 dependency) |
| [Adafruit SHTC3 Library](https://github.com/adafruit/Adafruit_SHTC3) | Reads the onboard temperature/humidity sensor |
| [SensorLib](https://github.com/lewisxhe/SensorLib) | Drives the onboard PCF85063 real-time clock |

All are pulled in automatically by `platformio.ini` — no manual library
installation needed.

## A note on how this was put together

I don't have one of these boards on hand to test against, so instead of
guessing at pin numbers and behavior, I read Waveshare's own example
firmware for this exact product (their `11_RTC_Sleep_Test` Arduino example)
line by line, along with the actual source of every library this project
uses (GxEPD2, Adafruit SHTC3, SensorLib, and the ESP32 Arduino core itself),
to confirm every pin, register address, and API call used here. I wasn't
able to run a live PlatformIO build in this environment (its package
registry isn't reachable from here), so I could not compile-test it myself.
Flag anything that doesn't build or behave as expected and I'll fix it.
