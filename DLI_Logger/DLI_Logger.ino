/**
 * DLI_Logger.ino
 * Plant Light Logger — main sketch (headless: no display, no menus yet).
 *
 * Samples the AS7341 once per second, integrates PAR into a running Daily
 * Light Integral, and appends to the SD card:
 *   HOURLY.CSV  one row at each hour rollover
 *   DAILY.CSV   one row at each midnight rollover, then the DLI counter resets
 * A status line is printed to Serial every STATUS_MS.
 *
 * Needs CAL.CFG on the SD card (run Calibration.ino first).  Without it the
 * logger still runs but PAR / DLI read 0, so a warning is printed at boot and
 * with every status line.
 *
 * Failure behaviour — the logger keeps running and recovers:
 *   AS7341 or DS3231 missing  -> retried every RETRY_MS; nothing is sampled /
 *                                logged until both are present
 *   SD card missing / failing -> sampling and DLI continue; the card is
 *                                re-initialised every RETRY_MS
 *
 * Known limitation: the running DLI lives in RAM.  A reset or power loss
 * mid-day restarts today's DLI from zero, so that day's DAILY.CSV row (and
 * the partial hour's row) under-counts.  The "samples" column shows it.
 *
 * Wiring (Mega 2560):
 *   AS7341 + DS3231  SDA -> 20   SCL -> 21   (5V / GND)
 *   SD card          CS -> 4   MOSI -> 51   MISO -> 50   SCK -> 52
 *   TFT stays connected but unused; its CS (pin 7) is driven HIGH so it
 *   ignores the SD traffic on the shared SPI bus.
 *
 * Serial Monitor: 115200 baud.
 *
 * Libraries: Adafruit AS7341, RTClib, SdFat (all via Library Manager)
 *
 * NOTE: copy DataManager.h and TimeManager.h from lib/ into this sketch
 *       folder before compiling.
 */

#include <Wire.h>
#include <SPI.h>
#include "TimeManager.h"
#include "DataManager.h"

#define SD_CS    4
#define TFT_CS   7

static const char* LOCATION = "DEFAULT_LOC";    // later: SettingsManager

static const unsigned long SAMPLE_MS = 1000;
static const unsigned long STATUS_MS = 10000;
static const unsigned long RETRY_MS  = 30000;

TimeManager timeMgr;
DataManager dataMgr;

bool          rtcOk       = false;
bool          sensorOk    = false;
bool          sdNeedsInit = false;     // a write failed; re-init the card
unsigned long lastSampleMs = 0;
unsigned long lastStatusMs = 0;
unsigned long lastRetryMs  = 0;

static void printTime(const DateTime& t) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
             (unsigned)t.year(), (unsigned)t.month(), (unsigned)t.day(),
             (unsigned)t.hour(), (unsigned)t.minute(), (unsigned)t.second());
    Serial.print(buf);
}

static void printStatus() {
    printTime(timeMgr.getCurrentTime());
    Serial.print(F("  PAR="));  Serial.print(dataMgr.getCurrentPAR(), 2);
    Serial.print(F("  DLI="));  Serial.print(dataMgr.getCalculatedDLI(), 5);
    Serial.print(F("  "));
    if (dataMgr.isDark()) Serial.print(F("DARK"));
    else                  Serial.print(dataMgr.getLightName());
    if (dataMgr.isSaturated()) Serial.print(F(" SATURATED"));
    Serial.print(F("  g="));   Serial.print(dataMgr.getGainFactor(), 1);
    Serial.print(F("  SD="));  Serial.print(dataMgr.sdAvailable() ? F("ok") : F("DOWN"));
    Serial.print(F("  rdFail=")); Serial.print(dataMgr.getReadFailures());
    Serial.print(F("  wrFail=")); Serial.print(dataMgr.getWriteFailures());
    if (!dataMgr.hasCalibration()) Serial.print(F("  NO CAL.CFG"));
    Serial.println();
}

// Re-attempt whatever failed at boot or since.  Cheap when everything is up.
static void retryInits() {
    if (!rtcOk) {
        rtcOk = timeMgr.init();
        Serial.println(rtcOk ? F("RTC recovered") : F("RTC still missing"));
    }
    if (!sensorOk) {
        sensorOk = dataMgr.initSensor();
        Serial.println(sensorOk ? F("AS7341 recovered") : F("AS7341 still missing"));
    }
    if (!dataMgr.sdAvailable() || sdNeedsInit) {
        sdNeedsInit = false;
        bool ok = dataMgr.initSD();
        Serial.println(ok ? F("SD recovered") : F("SD still down"));
    }
}

static void checkWrite(const __FlashStringHelper* what, bool ok) {
    if (ok) return;
    Serial.print(F("!! ")); Serial.print(what); Serial.println(F(" write FAILED"));
    sdNeedsInit = true;
}

void setup() {
    // Deselect both SPI devices before any SPI traffic.
    pinMode(SD_CS, OUTPUT);   digitalWrite(SD_CS, HIGH);
    pinMode(TFT_CS, OUTPUT);  digitalWrite(TFT_CS, HIGH);

    Serial.begin(115200);
    Wire.begin();

    Serial.println(F("--- DLI Logger ---"));
    rtcOk    = timeMgr.init();
    sensorOk = dataMgr.initSensor();
    dataMgr.initSD();

    Serial.print(F("RTC:    ")); Serial.println(rtcOk ? F("OK") : F("NOT FOUND"));
    Serial.print(F("AS7341: ")); Serial.println(sensorOk ? F("OK") : F("NOT FOUND"));
    Serial.print(F("SD:     ")); Serial.println(dataMgr.sdAvailable() ? F("OK") : F("INIT FAILED"));
    if (dataMgr.hasCalibration()) {
        Serial.print(F("K_sensor=")); Serial.println(dataMgr.getKSensor(), 8);
    } else {
        Serial.println(F("WARNING: no CAL.CFG - PAR and DLI will read 0. Run Calibration.ino."));
    }
    if (timeMgr.wasClockReset()) {
        Serial.println(F("WARNING: RTC lost power - time is only the compile time."));
    }
    Serial.print(F("Location: ")); Serial.println(LOCATION);

    lastRetryMs = millis();
}

void loop() {
    unsigned long now = millis();

    // Hardware recovery.
    if ((!rtcOk || !sensorOk || !dataMgr.sdAvailable() || sdNeedsInit) &&
        now - lastRetryMs >= RETRY_MS) {
        lastRetryMs = now;
        retryInits();
    }

    // Everything below needs a clock and a sensor.
    if (!rtcOk || !sensorOk) return;

    // Sample once per second and integrate.
    if (now - lastSampleMs >= SAMPLE_MS) {
        float dt = (lastSampleMs == 0) ? SAMPLE_MS / 1000.0f
                                       : (now - lastSampleMs) / 1000.0f;
        lastSampleMs = now;
        if (dataMgr.captureInstantaneousSpectrum()) {
            dataMgr.processDLIIntegration(dt);
        } else {
            Serial.println(F("!! sensor read failed"));
        }
    }

    // Rollovers.  At midnight both fire; the hourly row goes first so it
    // carries the full day's DLI, then the daily row closes yesterday.
    if (timeMgr.hasHourRolledOver()) {
        DateTime t = timeMgr.getCurrentTime();
        Serial.print(F("HOUR rollover ")); printTime(t); Serial.println();
        checkWrite(F("HOURLY.CSV"), dataMgr.commitHourlyRecordToSD(LOCATION, t));
    }
    if (timeMgr.hasDayRolledOver()) {
        DateTime t = timeMgr.getCurrentTime();
        Serial.print(F("DAY rollover ")); printTime(t); Serial.println();
        checkWrite(F("DAILY.CSV"),
                   dataMgr.rolloverDailyLog(LOCATION, t - TimeSpan(1, 0, 0, 0)));
        dataMgr.resetDLICounter();
    }

    if (now - lastStatusMs >= STATUS_MS) {
        lastStatusMs = now;
        printStatus();
    }
}
