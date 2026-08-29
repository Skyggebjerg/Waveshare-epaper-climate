/*
 * Waveshare ESP32-S3-ePaper-1.54 — battery-friendly temperature/humidity display
 * ------------------------------------------------------------------------
 * Board:   https://docs.waveshare.com/ESP32-S3-ePaper-1.54
 *          1.54" 200x200 monochrome e-paper (SSD1681), onboard SHTC3
 *          temperature/humidity sensor and PCF85063 real-time clock.
 *
 * What this does:
 *   1. Wakes up (from a cold boot, from the 2-minute timer, or because
 *      the BOOT button was pressed).
 *   2. Reads the onboard SHTC3 sensor and the PCF85063 RTC.
 *   3. Draws temperature, humidity, the current time and battery level
 *      on the e-paper.
 *   4. Goes back into deep sleep for SLEEP_SECONDS. Pressing BOOT while
 *      asleep wakes it immediately instead of waiting out the interval.
 *
 * NOTE ON USB: the native USB port fully drops out during deep sleep (the
 * USB peripheral loses power along with everything else) and only comes
 * back once the chip actually wakes and re-boots. If you need to upload
 * new code while it's asleep, hold BOOT, tap RESET, then release BOOT —
 * this forces USB download mode regardless of sleep state (see README).
 *
 * Almost nothing runs in loop() — everything happens once in setup(),
 * then the chip sleeps. This keeps average power draw low, which matters
 * because the board runs from a small LiPo cell.
 *
 * This firmware is the result of a hands-on debugging session against the
 * real hardware — see NOTES.md in this project for the full story of how
 * the sensor/RTC reading was finally made to work (short version: the
 * lewisxhe/SensorLib library's RTC self-test was failing on this exact
 * board; swapping to the Soldered PCF85063A library fixed it). Pin map
 * and the GPIO17/GPIO6 power-latch behaviour come from Waveshare's own
 * example firmware for this board (RTC_Sleep_Test); the GPIO3 line has no
 * confirmed explanation but is cheap to keep — see NOTES.md.
 *
 * PARTIAL UPDATE / FRAME DIFFING (confirmed working on hardware):
 * To cut down on the full-screen flashing on every wake, each frame is
 * first rendered into an off-screen canvas, then compared pixel-for-pixel
 * against a copy of the previously-shown frame kept in RTC memory (which
 * survives deep sleep - 5000 bytes, most of the ESP32-S3's 8KB RTC
 * budget). Only the bounding box of the pixels that actually differ is
 * pushed to the panel as a partial (non-flashing) update; if nothing
 * differs, the panel isn't touched at all. Because the diff works on the
 * real pixels rather than a hand-maintained table of screen regions, the
 * layout (baselines, positions, fonts) can be rearranged freely without
 * breaking partial updates.
 * Partial refreshes leave residue behind ("ghosting"/graininess -
 * confirmed on this panel), so after every FULL_REFRESH_EVERY partial
 * updates one full flashing refresh is forced to clean the panel.
 * NOTE: GxEPD2 is initialized with initial_refresh=false when a
 * remembered frame exists - normally that mechanism assumes the panel's
 * power was never cut, whereas this firmware cuts it every cycle for
 * battery life; empirically it works well on this exact board anyway.
 */

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <math.h>   // NAN
#include <stdio.h>  // sscanf
#include <string.h> // strstr, strcmp, strncpy

#include <GxEPD2_BW.h>
// Orbitron fonts (Adafruit GFX format). These header files must sit next
// to this file in src/ (or anywhere on the include path). The symbol name
// inside each header is assumed to match its filename (the usual
// fontconvert/truetype2gfx convention) - if your headers name the GFXfont
// struct differently, adjust the &Orbitron_* references below to match.
#include "Orbitron_Bold_70.h"    // large number - the RH hero reading
#include "Orbitron_Bold_32.h"    // medium - temperature and the % sign
#include "Orbitron_Medium_20.h"  // small - time, date, battery, labels

#include <Adafruit_SHTC3.h>
#include <PCF85063A-SOLDERED.h> // Soldered PCF85063A RTC library

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"
#include "esp_attr.h" // RTC_DATA_ATTR

// ---------------------------------------------------------------------
// Board pin map (Waveshare ESP32-S3-ePaper-1.54)
// ---------------------------------------------------------------------
static const int EPD_CS_PIN   = 11;
static const int EPD_DC_PIN   = 10;
static const int EPD_RST_PIN  = 9;
static const int EPD_BUSY_PIN = 8;
static const int EPD_SCK_PIN  = 12;
static const int EPD_MOSI_PIN = 13;
static const int EPD_PWR_PIN  = 6;  // e-paper panel power switch: LOW = on, HIGH = off

static const int VBAT_PWR_PIN = 17; // battery power latch: must be driven HIGH to keep
                                     // the board powered; if this ever goes low the
                                     // board switches itself off (same as unplugging it).

// Onboard LED. Doubles as an "awake" indicator: on for the whole time
// the chip is up and running (from latchBoardPower() below through to
// goToSleep()), off through deep sleep. It was originally added here for
// an unrelated, never-fully-confirmed reason (see NOTES.md, "Unexplained
// but proven") - driving it HIGH at boot, alongside the two power pins,
// is something an independent working sketch for this exact board does
// too - but it happens to line up exactly with "on while awake" already.
static const int LED_PIN = 3;

static const int I2C_SDA_PIN = 47;
static const int I2C_SCL_PIN = 48;

static const int BOOT_BUTTON_PIN = 0; // wired to ground when pressed

// Battery voltage sense: GPIO4 (ADC1 channel 3), behind a 200k/200k
// divider (so the real battery voltage is double whatever the ADC pin
// reads). Confirmed by both Waveshare's own factory firmware
// (port_adc.cpp, Get_VbatVoltage()/Get_Batterylevel()) and an independent
// working sketch for this exact board - same pin, same 2x multiplier.
static const int BATTERY_ADC_PIN = 4;

// ---------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------
static const uint64_t SLEEP_SECONDS = 300;   // 2 minutes between updates
static const uint32_t AWAKE_MILLIS = 15000;  // how long to stay awake with the
                                              // reading on screen before sleeping again
// Partial refreshes never fully reset the panel's pixels, so residue
// ("ghosting"/graininess) builds up over many cycles. Every this-many
// partial updates, one full flashing refresh is forced to clean it off.
// Lower = cleaner screen but more flashing; raise it if the flashing
// bothers you more than the graininess.
static const uint32_t FULL_REFRESH_EVERY = 15;
static const int SPI_CLOCK_HZ = 4000000;

// ---------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------
GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT>
    display(GxEPD2_154_D67(EPD_CS_PIN, EPD_DC_PIN, EPD_RST_PIN, EPD_BUSY_PIN));

Adafruit_SHTC3 shtc3;
PCF85063A rtc;

bool sensorOk = false;

// ---------------------------------------------------------------------
// Remembered previous frame (RTC memory - survives deep sleep, reset on
// a real power-on/reset). A complete 1-bit copy of what the panel is
// showing: 200x200 / 8 = 5000 bytes, most of the ESP32-S3's 8KB RTC
// memory budget. Each new frame is rendered into an off-screen canvas
// and diffed against this pixel-for-pixel, so the changed region is
// computed from the actual pixels - the layout (baselines, positions,
// fonts) can be rearranged freely without breaking partial updates.
// ---------------------------------------------------------------------
static const int SCREEN_W = 200;
static const int SCREEN_H = 200;
static const int SCREEN_ROW_BYTES = SCREEN_W / 8; // 25
static const uint32_t PREV_FRAME_MAGIC = 0x45504432; // "EPD2" - bumped when the diff scheme changed

RTC_DATA_ATTR uint32_t prevFrameMagic = 0;
RTC_DATA_ATTR uint32_t partialCyclesSinceFull = 0; // ghosting control - see FULL_REFRESH_EVERY
RTC_DATA_ATTR uint8_t prevFrame[SCREEN_ROW_BYTES * SCREEN_H]; // 5000 bytes

// Off-screen canvas each new frame is composed on before being diffed
// against prevFrame and pushed to the panel.
// Convention: bit set (color 1) = black ink, bit clear = white paper.
GFXcanvas1 frameCanvas(SCREEN_W, SCREEN_H);

struct ScreenRect { int16_t x, y, w, h; };

// Vertical layout. Move any of these freely - the pixel diff picks up
// whatever actually changed, so nothing else needs to be kept in sync.
// Everything except the top strip is horizontally centered at draw time
// (measured with getTextBounds).
static const int16_t TOP_BASELINE     = 22;  // time + battery %, Medium_20
static const int16_t HERO_BASELINE    = 112; // RH number and % sign, Bold_70/Bold_32
static const int16_t HERO_LABEL_DROP  = 36;  // "RH" label baseline sits this far above HERO_BASELINE
static const int16_t TEMP_BASELINE    = 162; // temperature, Bold_32
static const int16_t DATE_BASELINE    = 196; // date, Medium_20

// ---------------------------------------------------------------------
// Battery
// ---------------------------------------------------------------------
float readBatteryVoltage()
{
    analogReadResolution(12);
    analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
    int mv = analogReadMilliVolts(BATTERY_ADC_PIN);
    return (mv / 1000.0f) * 2.0f; // undo the 200k/200k divider
}

// Same thresholds Waveshare's own factory firmware uses (Get_Batterylevel()) -
// this board's charge circuit tops out a bit under the textbook 4.2V for a
// LiPo cell, so 0-100% is mapped against 3.0V-4.12V rather than 3.0-4.2V.
int batteryPercentFromVoltage(float volts)
{
    const float VOLT_FULL = 4.12f;
    const float VOLT_EMPTY = 3.0f;
    if (volts <= VOLT_EMPTY) return 0;
    if (volts >= VOLT_FULL) return 100;
    return (int)((volts - VOLT_EMPTY) / (VOLT_FULL - VOLT_EMPTY) * 100.0f);
}

// ---------------------------------------------------------------------
// RTC: detect whether it needs to be set, and set it from the firmware's
// own build time if so.
//
// The Soldered library's own getters mask off and discard the PCF85063's
// "oscillator stopped" status bit, so we peek at the raw seconds register
// ourselves just for that one bit - it's the only reliable way to know
// whether the chip has ever lost backup power (as opposed to just showing
// an implausible year, which we also check as a backup signal).
// ---------------------------------------------------------------------
static const uint8_t PCF85063_ADDR       = 0x51;
static const uint8_t PCF85063_REG_SECOND = 0x04;

bool pcf85063LostBackupPower()
{
    Wire.beginTransmission(PCF85063_ADDR);
    Wire.write(PCF85063_REG_SECOND);
    if (Wire.endTransmission(false) != 0) {
        return true; // couldn't even read it - don't trust it
    }
    if (Wire.requestFrom((int)PCF85063_ADDR, 1) != 1) {
        return true;
    }
    uint8_t secondsReg = Wire.read();
    return (secondsReg & 0x80) != 0;
}

struct CompileTime {
    int year, month, day, hour, minute, second;
};

// Parses the compiler-provided __DATE__ ("Mmm dd yyyy") / __TIME__
// ("hh:mm:ss") strings, for the fallback below.
CompileTime parseCompileDateTime()
{
    static const char *monthNames = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char monStr[4] = {0};
    int day = 1, year = 2026, hour = 0, minute = 0, second = 0;
    sscanf(__DATE__, "%3s %d %d", monStr, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second);
    const char *pos = strstr(monthNames, monStr);
    int month = pos ? (int)((pos - monthNames) / 3) + 1 : 1;

    CompileTime t;
    t.year = year;
    t.month = month;
    t.day = day;
    t.hour = hour;
    t.minute = minute;
    t.second = second;
    return t;
}

// The PCF85063 keeps running on its own backup power across our deep
// sleeps, so normally we do NOT want to touch its clock on every boot.
// Only fall back to the firmware's build time if the chip lost its
// backup power (brand new board, or the battery/backup cap was ever
// fully drained) and is therefore no longer keeping reliable time.
void initRtcIfNeeded()
{
    bool lostPower = pcf85063LostBackupPower();
    int currentYear = rtc.getYear(); // triggers a real read of the chip

    // Note: this library stores years as an offset from 1970 (not the
    // more common 2000), so 2024-2069 is the representable "looks like a
    // real date" range with this particular library's getYear()/setDate().
    bool yearLooksValid = (currentYear >= 2024 && currentYear <= 2069);

    if (lostPower || !yearLooksValid) {
        CompileTime t = parseCompileDateTime();
        rtc.setDate(0, t.day, t.month, t.year); // weekday unused, pass 0
        rtc.setTime(t.hour, t.minute, t.second);
        Serial.println("RTC time was not trustworthy - set from firmware build time");
    }
}

// ---------------------------------------------------------------------
// Keep the board powered and bring the e-paper's power rail back up.
// ---------------------------------------------------------------------
void latchBoardPower()
{
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis((gpio_num_t)VBAT_PWR_PIN);
    gpio_hold_dis((gpio_num_t)EPD_PWR_PIN);

    pinMode(VBAT_PWR_PIN, OUTPUT);
    digitalWrite(VBAT_PWR_PIN, HIGH); // keep battery power latched on

    pinMode(EPD_PWR_PIN, OUTPUT);
    digitalWrite(EPD_PWR_PIN, LOW);   // power the e-paper panel back up

    // Driving this HIGH (as an independent reference sketch for this board
    // does) didn't actually light it on the real hardware, so this now
    // tries the opposite polarity - LOW = on - which is the other common
    // wiring for a GPIO-driven status LED (GPIO sinks current through the
    // LED to turn it on, instead of sourcing it). If this still doesn't
    // light up, GPIO3 may not be a user-visible LED on this board at all.
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    delay(10);
}

// ---------------------------------------------------------------------
// Format the on-screen text for a reading. Kept separate from the actual
// drawing so the same strings can be compared against the remembered
// previous frame *before* we decide how to touch the display.
// ---------------------------------------------------------------------
struct ReadingLines {
    char timeLine[16];
    char dateLine[16];
    char tempLine[24];
    char humidityLine[24];
    char batteryLine[8];
};

void formatReadingLines(ReadingLines &lines, int hour, int minute, int day, int month, int year,
                         float tempC, float humidityRH, int batteryPercent)
{
    snprintf(lines.timeLine, sizeof(lines.timeLine), "%02d:%02d", hour, minute);
    snprintf(lines.dateLine, sizeof(lines.dateLine), "%02d/%02d/%04d", day, month, year);
    snprintf(lines.batteryLine, sizeof(lines.batteryLine), "%d%%", batteryPercent);

    if (sensorOk) {
        snprintf(lines.tempLine, sizeof(lines.tempLine), "%.1f C", tempC);
        // Just the number - the % and RH labels are drawn separately in
        // smaller fonts next to it (the hero font is too big for both).
        snprintf(lines.humidityLine, sizeof(lines.humidityLine), "%.0f", humidityRH);
    } else {
        snprintf(lines.tempLine, sizeof(lines.tempLine), "Sensor offline");
        snprintf(lines.humidityLine, sizeof(lines.humidityLine), "--");
    }
}

// ---------------------------------------------------------------------
// Render the complete frame into the off-screen canvas. Nothing touches
// the physical display here - this is pure drawing, so the result can be
// diffed against the previous frame before deciding what to push.
// Canvas convention: color 1 = black ink, 0 = white paper.
// ---------------------------------------------------------------------
void renderFrame(const ReadingLines &lines, int batteryPercent)
{
    frameCanvas.fillScreen(0);
    frameCanvas.setTextColor(1);

    // Measure everything that gets centered (getTextBounds needs the
    // right font selected on the canvas first).
    int16_t bx, by;
    uint16_t heroW, heroH, pctW, pctH, rhW, rhH, tempW, tempH, dateW, dateH;

    frameCanvas.setFont(&Orbitron_Bold_70);
    frameCanvas.getTextBounds(lines.humidityLine, 0, HERO_BASELINE, &bx, &by, &heroW, &heroH);
    frameCanvas.setFont(&Orbitron_Bold_32);
    frameCanvas.getTextBounds("%", 0, HERO_BASELINE, &bx, &by, &pctW, &pctH);
    frameCanvas.setFont(&Orbitron_Medium_20);
    frameCanvas.getTextBounds("RH", 0, HERO_BASELINE, &bx, &by, &rhW, &rhH);

    if (sensorOk) {
        frameCanvas.setFont(&Orbitron_Bold_32);
    } else {
        frameCanvas.setFont(&Orbitron_Medium_20); // "Sensor offline" is too wide for Bold_32
    }
    frameCanvas.getTextBounds(lines.tempLine, 0, TEMP_BASELINE, &bx, &by, &tempW, &tempH);
    frameCanvas.setFont(&Orbitron_Medium_20);
    frameCanvas.getTextBounds(lines.dateLine, 0, DATE_BASELINE, &bx, &by, &dateW, &dateH);

    // Hero group = big number + a right-hand column holding "RH" stacked
    // tightly over "%". The whole group is centered as one unit.
    const int16_t heroGap = 6; // gap between the number and the RH/% column
    uint16_t colW = max(pctW, rhW);
    int16_t heroX = (SCREEN_W - (int16_t)(heroW + heroGap + colW)) / 2;
    if (heroX < 0) heroX = 0;
    int16_t colX = heroX + heroW + heroGap;
    int16_t tempX = (SCREEN_W - (int16_t)tempW) / 2; if (tempX < 0) tempX = 0;
    int16_t dateX = (SCREEN_W - (int16_t)dateW) / 2; if (dateX < 0) dateX = 0;

    // --- Top strip: time (left), battery (right), separator below ---
    frameCanvas.setFont(&Orbitron_Medium_20);
    frameCanvas.setCursor(2, TOP_BASELINE);
    frameCanvas.print(lines.timeLine);

    // Battery icon, top-right corner, with the percentage to its left
    // (right-aligned against the icon so 2- and 3-digit values both fit).
    frameCanvas.drawRect(157, 6, 38, 16, 1);
    frameCanvas.fillRect(195, 10, 4, 8, 1); // nub
    int batterySegments = batteryPercent / 20;    // 0-5 bars
    if (batterySegments > 5) batterySegments = 5;
    for (int i = 0; i < batterySegments; i++) {
        frameCanvas.fillRect(160 + (i * 7), 9, 5, 10, 1);
    }
    int16_t batX, batY;
    uint16_t batW, batH;
    frameCanvas.getTextBounds(lines.batteryLine, 0, TOP_BASELINE, &batX, &batY, &batW, &batH);
    frameCanvas.setCursor(152 - (int16_t)batW, TOP_BASELINE);
    frameCanvas.print(lines.batteryLine);

    frameCanvas.drawFastHLine(0, 28, SCREEN_W, 1); // separator under the status strip

    // --- Hero: the RH reading, big and centered as one group ---
    frameCanvas.setFont(&Orbitron_Bold_70);
    frameCanvas.setCursor(heroX, HERO_BASELINE);
    frameCanvas.print(lines.humidityLine);

    // "%" bottom-aligned with the number; "RH" sitting tightly above
    // it, centered over the % so the column reads as one label.
    frameCanvas.setFont(&Orbitron_Bold_32);
    frameCanvas.setCursor(colX + (colW - pctW) / 2, HERO_BASELINE);
    frameCanvas.print("%");

    frameCanvas.setFont(&Orbitron_Medium_20);
    frameCanvas.setCursor(colX + (colW - rhW) / 2, HERO_BASELINE - HERO_LABEL_DROP);
    frameCanvas.print("RH");

    // --- Temperature, centered ---
    if (sensorOk) {
        frameCanvas.setFont(&Orbitron_Bold_32);
    } else {
        frameCanvas.setFont(&Orbitron_Medium_20);
    }
    frameCanvas.setCursor(tempX, TEMP_BASELINE);
    frameCanvas.print(lines.tempLine);

    // --- Date, centered ---
    frameCanvas.setFont(&Orbitron_Medium_20);
    frameCanvas.setCursor(dateX, DATE_BASELINE);
    frameCanvas.print(lines.dateLine);
}

// ---------------------------------------------------------------------
// Pixel diff: compare the freshly-rendered canvas against the previous
// frame remembered in RTC memory. Returns false if they're identical;
// otherwise returns true and fills 'out' with the bounding box of every
// pixel that differs (x/w byte-aligned, which suits the panel anyway).
// ---------------------------------------------------------------------
bool diffFrames(ScreenRect &out)
{
    const uint8_t *cur = frameCanvas.getBuffer();
    int minRow = SCREEN_H, maxRow = -1;
    int minByte = SCREEN_ROW_BYTES, maxByte = -1;

    for (int row = 0; row < SCREEN_H; row++) {
        const uint8_t *a = cur + row * SCREEN_ROW_BYTES;
        const uint8_t *b = prevFrame + row * SCREEN_ROW_BYTES;
        for (int col = 0; col < SCREEN_ROW_BYTES; col++) {
            if (a[col] != b[col]) {
                if (row < minRow) minRow = row;
                if (row > maxRow) maxRow = row;
                if (col < minByte) minByte = col;
                if (col > maxByte) maxByte = col;
            }
        }
    }
    if (maxRow < 0) return false;

    out.x = (int16_t)(minByte * 8);
    out.y = (int16_t)minRow;
    out.w = (int16_t)((maxByte - minByte + 1) * 8);
    out.h = (int16_t)(maxRow - minRow + 1);
    return true;
}

// ---------------------------------------------------------------------
// Push the rendered canvas to the e-paper panel - either the full screen
// (flashing refresh, resets every pixel) or just the given window
// (partial, non-flashing) - then remember this frame as the new baseline.
// ---------------------------------------------------------------------
void pushFrameToDisplay(bool fullWindow, const ScreenRect &window)
{
    display.setRotation(0);
    if (fullWindow) {
        display.setFullWindow();
    } else {
        display.setPartialWindow(window.x, window.y, window.w, window.h);
    }
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        // Draw the canvas's set bits (ink) in black over the white fill.
        display.drawBitmap(0, 0, frameCanvas.getBuffer(), SCREEN_W, SCREEN_H, GxEPD_BLACK);
    } while (display.nextPage());

    // Ghosting bookkeeping: a full-window update uses the full flashing
    // waveform, which resets every pixel and clears accumulated residue.
    if (fullWindow) {
        partialCyclesSinceFull = 0;
    } else {
        partialCyclesSinceFull++;
    }

    memcpy(prevFrame, frameCanvas.getBuffer(), sizeof(prevFrame));
    prevFrameMagic = PREV_FRAME_MAGIC;
}

// ---------------------------------------------------------------------
// Power everything down and let the ESP32-S3 sleep for SLEEP_SECONDS,
// or until the BOOT button is pressed - whichever comes first.
// ---------------------------------------------------------------------
void goToSleep()
{
    display.hibernate();             // put the SSD1681 controller into its own low-power mode
    digitalWrite(EPD_PWR_PIN, HIGH); // cut power to the panel while we sleep
    digitalWrite(LED_PIN, HIGH);     // LED off (LOW = on - see latchBoardPower())

    // Freeze both power pins so they survive deep sleep; without this the
    // board would switch itself off / lose the panel-power state.
    gpio_hold_en((gpio_num_t)VBAT_PWR_PIN);
    gpio_hold_en((gpio_num_t)EPD_PWR_PIN);
    gpio_deep_sleep_hold_en();

    esp_sleep_enable_timer_wakeup(SLEEP_SECONDS * 1000000ULL);

    // BOOT button wakes it immediately too, so you don't have to wait out
    // the full 2 minutes whenever you want a fresh reading.
    rtc_gpio_pulldown_dis((gpio_num_t)BOOT_BUTTON_PIN);
    rtc_gpio_pullup_en((gpio_num_t)BOOT_BUTTON_PIN);
    esp_sleep_enable_ext1_wakeup(1ULL << BOOT_BUTTON_PIN, ESP_EXT1_WAKEUP_ANY_LOW);

    esp_deep_sleep_start(); // does not return
}

void printWakeReason()
{
    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_TIMER:
            Serial.println("Woke up: 2-minute timer");
            break;
        case ESP_SLEEP_WAKEUP_EXT1:
            Serial.println("Woke up: BOOT button press");
            break;
        default:
            Serial.println("Woke up: power-on / reset");
            break;
    }
}

void setup()
{
    Serial.begin(115200);
    // Give the native USB port a moment to enumerate so early prints aren't
    // lost when a laptop is connected. Has no effect when running on
    // battery alone (no host to wait for).
    unsigned long usbWaitStart = millis();
    while (!Serial && millis() - usbWaitStart < 2000) {
        delay(10);
    }
    Serial.println("\n--- boot ---");
    printWakeReason();

    latchBoardPower();

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    rtc.begin();
    initRtcIfNeeded();

    sensorOk = shtc3.begin(&Wire);
    if (!sensorOk) {
        Serial.println("SHTC3 sensor not found - check wiring");
    }

    int hour = rtc.getHour();
    int minute = rtc.getMinute();
    int day = rtc.getDay();
    int month = rtc.getMonth();
    int year = rtc.getYear();

    float tempC = NAN;
    float humidityRH = NAN;
    if (sensorOk) {
        sensors_event_t humidity, tempEvent;
        bool ok = shtc3.getEvent(&humidity, &tempEvent);
        if (!ok) {
            delay(5);
            ok = shtc3.getEvent(&humidity, &tempEvent);
        }
        if (ok) {
            tempC = tempEvent.temperature;
            humidityRH = humidity.relative_humidity;
        }
    }

    float batteryVoltage = readBatteryVoltage();
    int batteryPercent = batteryPercentFromVoltage(batteryVoltage);

    Serial.printf("%02d:%02d %02d/%02d/%04d   %.1f C, %.0f %%RH   battery %.2fV (%d%%)\n",
                  hour, minute, day, month, year, tempC, humidityRH, batteryVoltage, batteryPercent);

    // Only trust the remembered previous frame if we actually woke from
    // our own deep sleep (not a fresh power-on/reset, which resets RTC
    // memory anyway, but this is a cheap extra safety check).
    esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
    bool wokeFromDeepSleep = (wakeupCause == ESP_SLEEP_WAKEUP_TIMER || wakeupCause == ESP_SLEEP_WAKEUP_EXT1);
    bool havePrevFrame = wokeFromDeepSleep && (prevFrameMagic == PREV_FRAME_MAGIC);

    ReadingLines lines;
    formatReadingLines(lines, hour, minute, day, month, year, tempC, humidityRH, batteryPercent);

    // Every FULL_REFRESH_EVERY partial updates, force one full flashing
    // refresh to clear the residue ("ghosting") that partial updates
    // leave behind - without this the screen slowly turns grainy.
    bool forceFullRefresh = !havePrevFrame || (partialCyclesSinceFull >= FULL_REFRESH_EVERY);
    if (havePrevFrame && forceFullRefresh) {
        Serial.printf("Forcing a full refresh to clear ghosting (%lu partial updates since the last one)\n",
                      (unsigned long)partialCyclesSinceFull);
    }

    // Render the new frame off-screen, then diff it pixel-for-pixel
    // against the remembered previous frame to find what changed.
    renderFrame(lines, batteryPercent);

    ScreenRect changedWindow;
    bool pixelsChanged = true; // no previous frame = everything counts as changed
    if (havePrevFrame) {
        pixelsChanged = diffFrames(changedWindow);
    }
    bool needsUpdate = forceFullRefresh || pixelsChanged;

    if (needsUpdate) {
        if (havePrevFrame && !forceFullRefresh) {
            Serial.printf("Partial update: %dx%d px at (%d,%d)\n",
                          changedWindow.w, changedWindow.h, changedWindow.x, changedWindow.y);
        }
        SPI.begin(EPD_SCK_PIN, -1, EPD_MOSI_PIN, EPD_CS_PIN);
        display.epd2.selectSPI(SPI, SPISettings(SPI_CLOCK_HZ, MSBFIRST, SPI_MODE0));
        // initial_refresh=false tells GxEPD2 to trust the panel already
        // shows a valid image and skip its usual full clear on init, so
        // it goes straight into partial-update mode. Only done when we
        // actually have a remembered previous frame to diff against -
        // see the note about this near the top of the file. (A forced
        // anti-ghosting refresh still inits this way - pushing with the
        // full window is what triggers the flashing waveform.)
        display.init(115200, !havePrevFrame, 20, false);
        pushFrameToDisplay(forceFullRefresh, changedWindow);
    } else {
        Serial.println("Nothing changed since last wake - skipping display update");
    }

    Serial.printf("staying awake for %lums with the reading on screen, then sleeping for %llus\n",
                  (unsigned long)AWAKE_MILLIS, (unsigned long long)SLEEP_SECONDS);
    delay(AWAKE_MILLIS); // keep the just-drawn reading up (and USB/Serial alive) for a bit before sleeping
    goToSleep();
}

void loop()
{
    // Never reached: setup() ends in deep sleep.
}