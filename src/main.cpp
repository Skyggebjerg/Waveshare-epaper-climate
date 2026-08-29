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
 * PARTIAL UPDATE / "VALUE REMEMBERING" - EXPERIMENTAL, NOT YET VERIFIED
 * ON HARDWARE:
 * To cut down on the full-screen flashing on every wake, this build
 * remembers what was last drawn (in RTC memory, which survives deep
 * sleep) and, from the second wake onward, only asks GxEPD2 to refresh
 * the smallest band of the screen covering whatever actually changed
 * (see computeChangedWindow() / the ScreenRect table below), using
 * GxEPD2's documented `initial_refresh=false` mechanism instead of its
 * usual full clear.
 * CAVEAT: that mechanism is normally used when the e-paper panel's own
 * power was never cut - only the MCU deep-slept. Here EPD_PWR_PIN is cut
 * every single cycle (deliberately, for battery life), which also wipes
 * the SSD1681 controller's own internal comparison RAM - something
 * "remembering values" in the firmware cannot, by itself, restore. In
 * practice this may work fine, may show a brief artifact on the changed
 * area right after a wake, or may simply keep flashing regardless - this
 * hasn't been tested on the real board yet. If it misbehaves, the
 * reliable fix is the other option discussed: stop cutting panel power
 * during sleep so the controller's own RAM survives.
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
static const uint64_t SLEEP_SECONDS = 120;   // 2 minutes between updates
static const uint32_t AWAKE_MILLIS = 30000;  // how long to stay awake with the
                                              // reading on screen before sleeping again
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
// Remembered previous frame (RTC memory - survives deep sleep, but is
// reset to these initializers on a real power-on/reset). Used purely to
// work out which on-screen fields changed since last wake, so only that
// area needs a partial refresh. See the big caveat about this near the
// top of the file.
// ---------------------------------------------------------------------
static const uint32_t PREV_FRAME_MAGIC = 0x45504431; // "EPD1"

RTC_DATA_ATTR uint32_t prevFrameMagic = 0;
RTC_DATA_ATTR char prevTimeLine[16] = "";
RTC_DATA_ATTR char prevDateLine[16] = "";
RTC_DATA_ATTR char prevTempLine[24] = "";
RTC_DATA_ATTR char prevHumidityLine[24] = "";
RTC_DATA_ATTR char prevBatteryLine[8] = "";

// Fixed screen regions for each field, used to build the smallest
// bounding box that covers whatever changed. These just need to fully
// contain what drawReadings() draws at each cursor position below - they
// don't need to be pixel-tight.
// Padded generously relative to each font's actual glyph metrics - a few
// extra blank pixels pushed along for nothing costs little, whereas
// clipping part of a digit would be a visible bug.
//
// Layout (200x200, RH is the hero):
//   y   0- 29  top strip: time (left, Medium_20) + battery (right)
//   y  30-147  hero: RH number in Bold_70, centered, with % / RH beside it
//   y 148-177  temperature in Bold_32
//   y 178-199  date in Medium_20
struct ScreenRect { int16_t x, y, w, h; };
static const ScreenRect TIME_RECT      = {  0,   0, 100, 30 };
static const ScreenRect BATTERY_RECT   = { 100,  0, 100, 30 };
static const ScreenRect HUMIDITY_RECT  = {  0,  30, 200, 114 };
static const ScreenRect TEMP_RECT      = {  0, 144, 200, 34 };
static const ScreenRect DATE_RECT      = {  0, 178, 200, 22 };

// Baselines used by drawReadings() - kept next to the rects above so the
// two stay in sync if the layout is ever rearranged. The hero group, the
// temperature and the date are all horizontally centered at draw time
// (measured with getTextBounds), so only vertical positions live here.
static const int16_t TOP_BASELINE     = 22;  // time + battery %, Medium_20
static const int16_t HERO_BASELINE    = 112; // RH number and % sign, Bold_70/Bold_32
static const int16_t HERO_LABEL_DROP  = 36;  // "RH" label baseline sits this far above HERO_BASELINE
static const int16_t TEMP_BASELINE    = 162; // temperature, Bold_32
static const int16_t DATE_BASELINE    = 196; // date, Medium_20

ScreenRect unionRect(const ScreenRect &a, const ScreenRect &b)
{
    int16_t x1 = min(a.x, b.x);
    int16_t y1 = min(a.y, b.y);
    int16_t x2 = max((int16_t)(a.x + a.w), (int16_t)(b.x + b.w));
    int16_t y2 = max((int16_t)(a.y + a.h), (int16_t)(b.y + b.h));
    return ScreenRect{ x1, y1, (int16_t)(x2 - x1), (int16_t)(y2 - y1) };
}

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

// Compares 'lines' against the remembered previous frame. Returns false
// if every field is identical (nothing to redraw); otherwise returns
// true and fills 'out' with the smallest rectangle covering every field
// that changed.
bool computeChangedWindow(const ReadingLines &lines, ScreenRect &out)
{
    bool any = false;
    auto includeIfChanged = [&](const char *prev, const char *cur, const ScreenRect &r) {
        if (strcmp(prev, cur) != 0) {
            out = any ? unionRect(out, r) : r;
            any = true;
        }
    };
    includeIfChanged(prevTimeLine, lines.timeLine, TIME_RECT);
    includeIfChanged(prevDateLine, lines.dateLine, DATE_RECT);
    includeIfChanged(prevTempLine, lines.tempLine, TEMP_RECT);
    includeIfChanged(prevHumidityLine, lines.humidityLine, HUMIDITY_RECT);
    includeIfChanged(prevBatteryLine, lines.batteryLine, BATTERY_RECT);
    return any;
}

void rememberFrame(const ReadingLines &lines)
{
    strncpy(prevTimeLine, lines.timeLine, sizeof(prevTimeLine));
    strncpy(prevDateLine, lines.dateLine, sizeof(prevDateLine));
    strncpy(prevTempLine, lines.tempLine, sizeof(prevTempLine));
    strncpy(prevHumidityLine, lines.humidityLine, sizeof(prevHumidityLine));
    strncpy(prevBatteryLine, lines.batteryLine, sizeof(prevBatteryLine));
    prevFrameMagic = PREV_FRAME_MAGIC;
}

// ---------------------------------------------------------------------
// Draw the current reading to the e-paper panel. The whole buffer is
// always fully redrawn (cheap - it's just text on a 200x200 mono
// buffer), regardless of window; 'fullWindow'/'window' only control how
// much of that buffer actually gets pushed to the physical panel.
// ---------------------------------------------------------------------
void drawReadings(const ReadingLines &lines, int batteryPercent, bool fullWindow, const ScreenRect &window)
{
    display.setRotation(0);
    if (fullWindow) {
        display.setFullWindow();
    } else {
        display.setPartialWindow(window.x, window.y, window.w, window.h);
    }
    // Measure everything that gets centered up front (getTextBounds needs
    // the right font selected, but not an open page).
    int16_t bx, by;
    uint16_t heroW, heroH, pctW, pctH, rhW, rhH, tempW, tempH, dateW, dateH;

    display.setFont(&Orbitron_Bold_70);
    display.getTextBounds(lines.humidityLine, 0, HERO_BASELINE, &bx, &by, &heroW, &heroH);
    display.setFont(&Orbitron_Bold_32);
    display.getTextBounds("%", 0, HERO_BASELINE, &bx, &by, &pctW, &pctH);
    display.setFont(&Orbitron_Medium_20);
    display.getTextBounds("RH", 0, HERO_BASELINE, &bx, &by, &rhW, &rhH);

    if (sensorOk) {
        display.setFont(&Orbitron_Bold_32);
    } else {
        display.setFont(&Orbitron_Medium_20); // "Sensor offline" is too wide for Bold_32
    }
    display.getTextBounds(lines.tempLine, 0, TEMP_BASELINE, &bx, &by, &tempW, &tempH);
    display.setFont(&Orbitron_Medium_20);
    display.getTextBounds(lines.dateLine, 0, DATE_BASELINE, &bx, &by, &dateW, &dateH);

    // Hero group = big number + a right-hand column holding "RH" stacked
    // tightly over "%". The whole group is centered as one unit.
    const int16_t heroGap = 6; // gap between the number and the RH/% column
    uint16_t colW = max(pctW, rhW);
    int16_t heroX = (200 - (int16_t)(heroW + heroGap + colW)) / 2;
    if (heroX < 0) heroX = 0;
    int16_t colX = heroX + heroW + heroGap;
    int16_t tempX = (200 - (int16_t)tempW) / 2; if (tempX < 0) tempX = 0;
    int16_t dateX = (200 - (int16_t)dateW) / 2; if (dateX < 0) dateX = 0;

    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);

        // --- Top strip: time (left), battery (right), separator below ---
        display.setFont(&Orbitron_Medium_20);
        display.setCursor(2, TOP_BASELINE);
        display.print(lines.timeLine);

        // Battery icon, top-right corner, with the percentage to its left
        // (right-aligned against the icon so 2- and 3-digit values both fit).
        display.drawRect(157, 6, 38, 16, GxEPD_BLACK);
        display.fillRect(195, 10, 4, 8, GxEPD_BLACK); // nub
        int batterySegments = batteryPercent / 20;    // 0-5 bars
        if (batterySegments > 5) batterySegments = 5;
        for (int i = 0; i < batterySegments; i++) {
            display.fillRect(160 + (i * 7), 9, 5, 10, GxEPD_BLACK);
        }
        int16_t batX, batY;
        uint16_t batW, batH;
        display.getTextBounds(lines.batteryLine, 0, TOP_BASELINE, &batX, &batY, &batW, &batH);
        display.setCursor(152 - (int16_t)batW, TOP_BASELINE);
        display.print(lines.batteryLine);

        display.drawFastHLine(0, 28, 200, GxEPD_BLACK); // separator under the status strip

        // --- Hero: the RH reading, big and centered as one group ---
        display.setFont(&Orbitron_Bold_70);
        display.setCursor(heroX, HERO_BASELINE);
        display.print(lines.humidityLine);

        // "%" bottom-aligned with the number; "RH" sitting tightly above
        // it, centered over the % so the column reads as one label.
        display.setFont(&Orbitron_Bold_32);
        display.setCursor(colX + (colW - pctW) / 2, HERO_BASELINE);
        display.print("%");

        display.setFont(&Orbitron_Medium_20);
        display.setCursor(colX + (colW - rhW) / 2, HERO_BASELINE - HERO_LABEL_DROP);
        display.print("RH");

        // --- Temperature, centered ---
        if (sensorOk) {
            display.setFont(&Orbitron_Bold_32);
        } else {
            display.setFont(&Orbitron_Medium_20);
        }
        display.setCursor(tempX, TEMP_BASELINE);
        display.print(lines.tempLine);

        // --- Date, centered ---
        display.setFont(&Orbitron_Medium_20);
        display.setCursor(dateX, DATE_BASELINE);
        display.print(lines.dateLine);
    } while (display.nextPage());

    rememberFrame(lines);
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

    ScreenRect changedWindow;
    bool needsUpdate = !havePrevFrame || computeChangedWindow(lines, changedWindow);

    if (needsUpdate) {
        SPI.begin(EPD_SCK_PIN, -1, EPD_MOSI_PIN, EPD_CS_PIN);
        display.epd2.selectSPI(SPI, SPISettings(SPI_CLOCK_HZ, MSBFIRST, SPI_MODE0));
        // initial_refresh=false tells GxEPD2 to trust the panel already
        // shows a valid image and skip its usual full clear on init, so
        // it goes straight into partial-update mode. Only done when we
        // actually have a remembered previous frame to diff against -
        // see the big caveat about this near the top of the file.
        display.init(115200, !havePrevFrame, 20, false);
        drawReadings(lines, batteryPercent, !havePrevFrame, changedWindow);
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