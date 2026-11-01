/**
 * TimeManagerTest.ino
 * Exercises TimeManager (DS3231 RTC wrapper) over the Serial Monitor.
 * No TFT, SD or other peripherals are needed — just the RTC on the I2C bus.
 *
 * Wiring (Mega 2560):
 *   DS3231 VCC -> 5V   GND -> GND   SDA -> 20   SCL -> 21
 *
 * Serial Monitor: 115200 baud, line ending "Newline".
 *
 * Commands:
 *   t                          print the current time now
 *   s YYYY MM DD HH MM SS      set the clock, e.g.  s 2026 11 1 14 30 0
 *   h                          jump to HH:59:55 of the current hour
 *                              (hour rollover fires ~5 s later)
 *   d                          jump to 23:59:55 of today
 *                              (hour AND day rollover fire ~5 s later)
 *   ?                          show this help
 *
 * The time prints once per second; rollover events print as they fire.
 *
 * Libraries: RTClib by Adafruit (accept the BusIO dependency)
 *
 * NOTE: copy TimeManager.h from lib/ into this sketch folder before compiling.
 */

#include <Wire.h>
#include "TimeManager.h"

TimeManager timeMgr;

unsigned long lastPrintMs = 0;

static void printTime(const DateTime& t) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
             t.year(), t.month(), t.day(), t.hour(), t.minute(), t.second());
    Serial.print(buf);
}

static void printHelp() {
    Serial.println(F("Commands:"));
    Serial.println(F("  t                         print time"));
    Serial.println(F("  s YYYY MM DD HH MM SS     set clock"));
    Serial.println(F("  h                         jump to HH:59:55 (hour rollover test)"));
    Serial.println(F("  d                         jump to 23:59:55 (day rollover test)"));
}

static void handleCommand(char* line) {
    switch (line[0]) {
        case 't':
            printTime(timeMgr.getCurrentTime());
            Serial.println();
            break;

        case 's': {
            int y, mo, d, h, mi, s;
            if (sscanf(line + 1, "%d %d %d %d %d %d", &y, &mo, &d, &h, &mi, &s) == 6) {
                timeMgr.setCurrentTime(y, mo, d, h, mi, s);
                Serial.print(F("Clock set to "));
                printTime(timeMgr.getCurrentTime());
                Serial.println();
            } else {
                Serial.println(F("Usage: s YYYY MM DD HH MM SS"));
            }
            break;
        }

        case 'h': {
            DateTime n = timeMgr.getCurrentTime();
            timeMgr.setCurrentTime(n.year(), n.month(), n.day(), n.hour(), 59, 55);
            Serial.print(F("Jumped to "));
            printTime(timeMgr.getCurrentTime());
            Serial.println(F("  -> expect HOUR rollover in ~5 s"));
            break;
        }

        case 'd': {
            DateTime n = timeMgr.getCurrentTime();
            timeMgr.setCurrentTime(n.year(), n.month(), n.day(), 23, 59, 55);
            Serial.print(F("Jumped to "));
            printTime(timeMgr.getCurrentTime());
            Serial.println(F("  -> expect HOUR + DAY rollover in ~5 s"));
            break;
        }

        case '?':
            printHelp();
            break;
    }
}

void setup() {
    Serial.begin(115200);
    Wire.begin();

    if (!timeMgr.init()) {
        Serial.println(F("FAIL: DS3231 not found - check SDA 20 / SCL 21 / power"));
        while (1);
    }
    Serial.println(F("RTC OK"));
    if (timeMgr.wasClockReset()) {
        Serial.println(F("WARNING: RTC lost power - set to compile time. Use 's' to set it."));
    }
    printHelp();
}

void loop() {
    // Rollover events: check every pass so they are reported promptly.
    if (timeMgr.hasHourRolledOver()) {
        Serial.print(F("*** HOUR rollover at "));
        printTime(timeMgr.getCurrentTime());
        Serial.println();
    }
    if (timeMgr.hasDayRolledOver()) {
        Serial.print(F("*** DAY rollover at "));
        printTime(timeMgr.getCurrentTime());
        Serial.println();
    }

    if (millis() - lastPrintMs >= 1000) {
        lastPrintMs = millis();
        printTime(timeMgr.getCurrentTime());
        Serial.println();
    }

    if (Serial.available()) {
        char line[40];
        size_t n = Serial.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        if (n > 0) handleCommand(line);
    }
}
