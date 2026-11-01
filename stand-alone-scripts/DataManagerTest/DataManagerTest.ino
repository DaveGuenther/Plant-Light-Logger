/**
 * DataManagerTest.ino
 * Exercises DataManager (AS7341 + SD card) and TimeManager (DS3231) together
 * over the Serial Monitor.  The key test is the SOAK: sampling the sensor at
 * 1 Hz while appending to the SD card, which is the combination the full
 * logger depends on.  No TFT output.
 *
 * Wiring (Mega 2560): same as the main logger.
 *   AS7341 + DS3231  SDA -> 20   SCL -> 21   (5V / GND)
 *   SD card          CS -> 4   MOSI -> 51   MISO -> 50   SCK -> 52
 *   TFT stays connected but unused; its CS (pin 7) is driven HIGH below so
 *   it ignores the SD traffic on the shared SPI bus.
 *
 * Needs a CAL.CFG on the SD card (run Calibration.ino first) for PAR / DLI
 * to be non-zero.  Without it the test still runs; PAR is reported as 0.
 *
 * Serial Monitor: 115200 baud, line ending "Newline".
 *
 * Commands:
 *   t   take one reading and print it
 *   s   start / stop LIVE sampling at 1 Hz (integrates DLI, prints each line)
 *   a   toggle ACCELERATED mode: while sampling, write an hourly record
 *       every 10 s instead of every hour (soak test)
 *   w   write an hourly record now
 *   d   write a daily record now (then resets the DLI counter)
 *   z   reset the DLI counter
 *   f   print HOURLY.CSV and DAILY.CSV
 *   x   delete HOURLY.CSV and DAILY.CSV
 *   h   jump clock to HH:59:55 (real hour rollover in ~5 s, needs 's' on)
 *   n   jump clock to 23:59:55 (hour + day rollover in ~5 s, needs 's' on)
 *   i   print status / failure counters
 *   ?   help
 *
 * Libraries: Adafruit AS7341, RTClib, SdFat (all via Library Manager)
 *
 * NOTE: copy DataManager.h AND TimeManager.h from lib/ into this sketch
 *       folder before compiling.
 */

#include <Wire.h>
#include <SPI.h>
#include "TimeManager.h"
#include "DataManager.h"

#define SD_CS    4
#define TFT_CS   7

static const char* LOC = "TEST_LOC";

static const unsigned long SAMPLE_MS      = 1000;
static const unsigned long ACCEL_RECORD_MS = 10000;

TimeManager timeMgr;
DataManager dataMgr;

bool          sampling   = false;
bool          accelMode  = false;
unsigned long lastSample = 0;
unsigned long lastAccel  = 0;
unsigned long sampleCount = 0;

static void printTime(const DateTime& t) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
             t.year(), t.month(), t.day(), t.hour(), t.minute(), t.second());
    Serial.print(buf);
}

static void printReading() {
    const uint16_t* ch = dataMgr.getRawSpectrum();
    for (uint8_t i = 0; i < 8; i++) { Serial.print(ch[i]); Serial.print(','); }
    Serial.print(F("g=")); Serial.print(dataMgr.getGainFactor(), 1);
    Serial.print(F(" PAR=")); Serial.print(dataMgr.getCurrentPAR(), 2);
    Serial.print(F(" DLI=")); Serial.print(dataMgr.getCalculatedDLI(), 5);
    Serial.print(F(" ")); Serial.print(dataMgr.getLightName());
    if (dataMgr.isSaturated()) Serial.print(F(" SATURATED"));
    if (dataMgr.isDark())      Serial.print(F(" DARK"));
    Serial.println();
}

static void printHelp() {
    Serial.println(F("t read | s live on/off | a accel on/off | w hourly rec | d daily rec"));
    Serial.println(F("z reset DLI | f dump files | x delete files | h/n jump clock | i status"));
}

static void printStatus() {
    Serial.print(F("sampling=")); Serial.print(sampling);
    Serial.print(F(" accel="));   Serial.print(accelMode);
    Serial.print(F(" samples=")); Serial.print(sampleCount);
    Serial.print(F(" hourSamples=")); Serial.println(dataMgr.getHourSamples());
    Serial.print(F("read failures="));  Serial.print(dataMgr.getReadFailures());
    Serial.print(F("  write failures=")); Serial.println(dataMgr.getWriteFailures());
}

static void dumpFile(const char* name) {
    Serial.print(F("--- ")); Serial.println(name);
    File32 f = dataMgr.fs().open(name, O_READ);
    if (!f) { Serial.println(F("(not found)")); return; }
    while (f.available()) Serial.write(f.read());
    f.close();
}

static void takeSample() {
    unsigned long now = millis();
    float dt = (lastSample == 0) ? SAMPLE_MS / 1000.0f : (now - lastSample) / 1000.0f;
    lastSample = now;

    if (dataMgr.captureInstantaneousSpectrum()) {
        dataMgr.processDLIIntegration(dt);
        sampleCount++;
        printReading();
    } else {
        Serial.println(F("!! sensor read failed"));
    }
}

static void report(const __FlashStringHelper* what, bool ok) {
    Serial.print(what); Serial.println(ok ? F(": OK") : F(": FAILED"));
}

static void handleCommand(char c) {
    switch (c) {
        case 't':
            if (dataMgr.captureInstantaneousSpectrum()) printReading();
            else Serial.println(F("!! sensor read failed"));
            break;

        case 's':
            sampling = !sampling;
            lastSample = 0;
            lastAccel = millis();
            Serial.println(sampling ? F("Live sampling ON") : F("Live sampling OFF"));
            if (!sampling) printStatus();
            break;

        case 'a':
            accelMode = !accelMode;
            lastAccel = millis();
            Serial.println(accelMode ? F("Accelerated records ON (every 10 s)")
                                     : F("Accelerated records OFF"));
            break;

        case 'w':
            report(F("Hourly record"), dataMgr.commitHourlyRecordToSD(LOC, timeMgr.getCurrentTime()));
            break;

        case 'd':
            report(F("Daily record"), dataMgr.rolloverDailyLog(LOC, timeMgr.getCurrentTime()));
            dataMgr.resetDLICounter();
            break;

        case 'z':
            dataMgr.resetDLICounter();
            Serial.println(F("DLI reset"));
            break;

        case 'f':
            dumpFile("HOURLY.CSV");
            dumpFile("DAILY.CSV");
            break;

        case 'x':
            dataMgr.fs().remove("HOURLY.CSV");
            dataMgr.fs().remove("DAILY.CSV");
            Serial.println(F("Files deleted"));
            break;

        case 'h': {
            DateTime n = timeMgr.getCurrentTime();
            timeMgr.setCurrentTime(n.year(), n.month(), n.day(), n.hour(), 59, 55);
            Serial.print(F("Clock -> ")); printTime(timeMgr.getCurrentTime());
            Serial.println(F("  (hour rollover in ~5 s)"));
            break;
        }

        case 'n': {
            DateTime n = timeMgr.getCurrentTime();
            timeMgr.setCurrentTime(n.year(), n.month(), n.day(), 23, 59, 55);
            Serial.print(F("Clock -> ")); printTime(timeMgr.getCurrentTime());
            Serial.println(F("  (hour + day rollover in ~5 s)"));
            break;
        }

        case 'i': printStatus(); break;
        case '?': printHelp();   break;
        default: break;
    }
}

void setup() {
    Serial.begin(115200);

    // Both SPI devices must be deselected before any SPI traffic, or the
    // one we are not talking to interprets the bytes as commands.
    pinMode(SD_CS, OUTPUT);   digitalWrite(SD_CS, HIGH);
    pinMode(TFT_CS, OUTPUT);  digitalWrite(TFT_CS, HIGH);

    Wire.begin();

    Serial.println(F("--- DataManager test ---"));

    bool rtcOk = timeMgr.init();
    Serial.print(F("RTC:    ")); Serial.println(rtcOk ? F("OK") : F("NOT FOUND"));

    bool sensorOk = dataMgr.initSensor();
    Serial.print(F("AS7341: ")); Serial.println(sensorOk ? F("OK") : F("NOT FOUND"));

    bool sdOk = dataMgr.initSD();
    Serial.print(F("SD:     ")); Serial.println(sdOk ? F("OK") : F("INIT FAILED"));

    if (dataMgr.hasCalibration()) {
        Serial.print(F("CAL.CFG: K_sensor=")); Serial.println(dataMgr.getKSensor(), 8);
    } else {
        Serial.println(F("CAL.CFG: not loaded (PAR will read 0)"));
    }

    if (!rtcOk || !sensorOk) { Serial.println(F("Halting: fix wiring.")); while (1); }
    printHelp();
}

void loop() {
    // Rollover events — only acted on while live sampling, like the real logger.
    if (timeMgr.hasHourRolledOver() && sampling) {
        DateTime now = timeMgr.getCurrentTime();
        Serial.print(F("*** HOUR rollover at ")); printTime(now); Serial.println();
        report(F("Hourly record"), dataMgr.commitHourlyRecordToSD(LOC, now));
    }
    if (timeMgr.hasDayRolledOver() && sampling) {
        DateTime now = timeMgr.getCurrentTime();
        Serial.print(F("*** DAY rollover at ")); printTime(now); Serial.println();
        report(F("Daily record"),
               dataMgr.rolloverDailyLog(LOC, now - TimeSpan(1, 0, 0, 0)));
        dataMgr.resetDLICounter();
    }

    if (sampling && millis() - lastSample >= SAMPLE_MS) {
        takeSample();
    }

    if (sampling && accelMode && millis() - lastAccel >= ACCEL_RECORD_MS) {
        lastAccel = millis();
        report(F("Accel hourly record"),
               dataMgr.commitHourlyRecordToSD(LOC, timeMgr.getCurrentTime()));
    }

    while (Serial.available()) {
        char c = Serial.read();
        if (c != '\n' && c != '\r') handleCommand(c);
    }
}
