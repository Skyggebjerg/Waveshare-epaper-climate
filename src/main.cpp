/*
 * Waveshare ESP32-S3-ePaper-1.54 — diagnostic build
 * ------------------------------------------------------------------------
 * This is a close port of a working, independent Arduino sketch for this
 * exact board: github.com/VolosR/waveshareEinkMonitor
 *
 * What's different from that sketch:
 *   - Its custom bitmap ("background.h") and custom fonts ("fonts.h") are
 *     replaced with plain built-in Adafruit_GFX fonts and a simple text
 *     layout, so there are no extra files to fetch.
 *   - Deep sleep is removed entirely, so USB/Serial stays up and you don't
 *     need to reconnect between tests. It reads once in setup(), then
 *     re-reads and re-prints (but does not redraw the screen) every 5
 *     seconds in loop(), purely so you can watch it on the Serial Monitor
 *     without having to reset the board for every attempt.
 *
 * Everything else - pin numbers, power-up sequence (including the GPIO3
 * line that has no confirmed explanation but costs nothing to keep),
 * Wire.begin() call, and which RTC/sensor libraries are used - is kept as
 * close to that reference sketch as possible, since it's the one piece of
 * independent evidence we have that this exact hardware works over I2C.
 */

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <math.h> // NAN

#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

#include <Adafruit_SHTC3.h>
#include <PCF85063A-SOLDERED.h> // Soldered PCF85063A RTC library

#include "driver/gpio.h"

// ----------- EPD pins (ESP32-S3) -----------
#define EPD_DC     10
#define EPD_CS     11
#define EPD_SCK    12
#define EPD_MOSI   13
#define EPD_RST     9
#define EPD_BUSY    8
#define EPD_PWR     6    // ACTIVE-LOW (ON = LOW)
#define VBAT_PWR   17    // rail enable (ON = HIGH)

// ----------- I2C pins -----------
#define I2C_SDA    47
#define I2C_SCL    48

// ----------- Settings -----------
#define SPI_CLOCK_HZ 4000000

// ----------- EPD (1.54" D67) -----------
GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(
    GxEPD2_154_D67(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

// ----------- Sensors / RTC -----------
Adafruit_SHTC3 shtc3;
PCF85063A rtc;

bool rtcOk = false;
bool sensorOk = false;

// ----------- Draw the reading (plain text, built-in fonts) -----------
void epdDraw(int hour, int minute, int day, int month, int year, float tempC, float humidityRH)
{
    char timeLine[16];
    char dateLine[16];
    char tempLine[24];
    char humidityLine[24];

    snprintf(timeLine, sizeof(timeLine), "%02d:%02d", hour, minute);
    snprintf(dateLine, sizeof(dateLine), "%02d/%02d/%04d", day, month, year);

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

        display.setFont(&FreeSansBold24pt7b);
        display.setCursor(10, 60);
        display.print(rtcOk ? timeLine : "--:--");

        display.setFont(&FreeSans12pt7b);
        display.setCursor(10, 95);
        display.print(rtcOk ? dateLine : "RTC offline");

        display.setFont(&FreeSansBold24pt7b);
        display.setCursor(10, 150);
        display.print(tempLine);

        display.setFont(&FreeSans12pt7b);
        display.setCursor(10, 185);
        display.print(humidityLine);
    } while (display.nextPage());
}

// ----------- Read sensor + RTC, print to Serial -----------
void readAndPrint(bool draw)
{
    int hour = 0, minute = 0, day = 0, month = 0;
    int year = 0;

    if (rtcOk) {
        hour = rtc.getHour();
        minute = rtc.getMinute();
        day = rtc.getDay();
        month = rtc.getMonth();
        year = rtc.getYear();
    }

    float tempC = NAN;
    float humidityRH = NAN;
    if (sensorOk) {
        sensors_event_t hum, temp;
        bool ok = shtc3.getEvent(&hum, &temp);
        if (!ok) {
            delay(5);
            ok = shtc3.getEvent(&hum, &temp);
        }
        if (ok) {
            tempC = temp.temperature;
            humidityRH = hum.relative_humidity;
        }
    }

    Serial.printf("RTC %s: %02d:%02d %02d/%02d/%04d   SHTC3 %s: %.1f C, %.0f %%RH\n",
                  rtcOk ? "ok" : "OFFLINE", hour, minute, day, month, year,
                  sensorOk ? "ok" : "OFFLINE", tempC, humidityRH);

    if (draw) {
        epdDraw(hour, minute, day, month, year, tempC, humidityRH);
    }
}

void setup()
{
    Serial.begin(115200);
    unsigned long usbWaitStart = millis();
    while (!Serial && millis() - usbWaitStart < 2000) {
        delay(10);
    }
    Serial.println("\n--- boot (no-sleep diagnostic build) ---");

    gpio_deep_sleep_hold_dis();
    gpio_hold_dis((gpio_num_t)VBAT_PWR);
    gpio_hold_dis((gpio_num_t)EPD_PWR);

    // turn on battery
    pinMode(VBAT_PWR, OUTPUT);
    digitalWrite(VBAT_PWR, HIGH);

    pinMode(EPD_PWR, OUTPUT);
    digitalWrite(EPD_PWR, LOW);

    pinMode(3, OUTPUT);
    digitalWrite(3, HIGH);

    delay(10);

    Wire.begin(I2C_SDA, I2C_SCL);
    rtc.begin();
    // A missing PCF85063 doesn't fail loudly here (this library's begin()
    // doesn't return a status), so we treat it as present and let the
    // actual reads speak for themselves in the Serial output below.
    rtcOk = true;

    sensorOk = shtc3.begin(&Wire);
    if (!sensorOk) {
        Serial.println("SHTC3 sensor not found - check wiring");
    }

    SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
    display.epd2.selectSPI(SPI, SPISettings(SPI_CLOCK_HZ, MSBFIRST, SPI_MODE0));
    display.init(115200);

    readAndPrint(true);

    Serial.println("--- staying awake, no deep sleep in this build ---");
}

void loop()
{
    // Re-read and re-print every 5s so you can watch it on the Serial
    // Monitor without resetting the board. Does not redraw the e-paper
    // panel repeatedly (full refreshes are slow and it's not needed for
    // this test).
    delay(5000);
    readAndPrint(false);
}