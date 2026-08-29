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
 */

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <math.h>   // NAN
#include <stdio.h>  // sscanf
#include <string.h> // strstr

#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

#include <Adafruit_SHTC3.h>
#include <PCF85063A-SOLDERED.h> // Soldered PCF85063A RTC library

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"

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
static const uint64_t SLEEP_SECONDS = 120; // 2 minutes between updates
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
// Draw the current reading to the e-paper panel
// ---------------------------------------------------------------------
void drawReadings(int hour, int minute, int day, int month, int year, float tempC, float humidityRH, int batteryPercent)
{
    char timeLine[16];
    char dateLine[16];
    char tempLine[24];
    char humidityLine[24];
    char batteryLine[8];

    snprintf(timeLine, sizeof(timeLine), "%02d:%02d", hour, minute);
    snprintf(dateLine, sizeof(dateLine), "%02d/%02d/%04d", day, month, year);
    snprintf(batteryLine, sizeof(batteryLine), "%d%%", batteryPercent);

    if (sensorOk) {
        snprintf(tempLine, sizeof(tempLine), "%.1f C", tempC);
        snprintf(humidityLine, sizeof(humidityLine), "RH %.0f %%", humidityRH);
    } else {
        snprintf(tempLine, sizeof(tempLine), "Sensor offline");
        humidityLine[0] = '\0';
    }

    display.setRotation(0);
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setTextColor(GxEPD_BLACK);

        // Battery icon + percentage, top-right corner.
        display.drawRect(150, 8, 40, 16, GxEPD_BLACK);
        display.drawRect(151, 9, 38, 14, GxEPD_BLACK);
        display.fillRect(190, 12, 3, 7, GxEPD_BLACK); // nub
        int batterySegments = batteryPercent / 20;    // 0-5 bars
        if (batterySegments > 5) batterySegments = 5;
        for (int i = 0; i < batterySegments; i++) {
            display.fillRect(154 + (i * 7), 12, 4, 8, GxEPD_BLACK);
        }
        display.setFont(&FreeSans9pt7b);
        display.setCursor(100, 21);
        display.print(batteryLine);

        display.setFont(&FreeSansBold24pt7b);
        display.setCursor(10, 60);
        display.print(timeLine);

        display.setFont(&FreeSans12pt7b);
        display.setCursor(10, 95);
        display.print(dateLine);

        display.setFont(&FreeSansBold24pt7b);
        display.setCursor(10, 150);
        display.print(tempLine);

        display.setFont(&FreeSans12pt7b);
        display.setCursor(10, 185);
        display.print(humidityLine);
    } while (display.nextPage());
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

    SPI.begin(EPD_SCK_PIN, -1, EPD_MOSI_PIN, EPD_CS_PIN);
    display.epd2.selectSPI(SPI, SPISettings(SPI_CLOCK_HZ, MSBFIRST, SPI_MODE0));
    display.init(115200);

    drawReadings(hour, minute, day, month, year, tempC, humidityRH, batteryPercent);

    Serial.println("staying awake for 30s with the reading on screen, then sleeping for 2 minutes");
    delay(30000); // keep the just-drawn reading up (and USB/Serial alive) for a bit before sleeping
    goToSleep();
}

void loop()
{
    // Never reached: setup() ends in deep sleep.
}