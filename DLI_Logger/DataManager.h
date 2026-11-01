/**
 * DataManager.h — header-only library
 * Plant Light Logger project
 *
 * Owns the AS7341 spectral sensor and the SD card.  Reads the 8 visible
 * channels with auto-gain, classifies the light source, converts to PAR using
 * the K_SENSOR constant saved by Calibration.ino, integrates PAR into a
 * running DLI, and appends hourly / daily records to CSV files on the card.
 *
 * Hardware:
 *   AS7341 on the shared I2C bus (SDA 20, SCL 21), address 0x39.
 *   SD card on shared SPI (MOSI 51, MISO 50, SCK 52), CS on pin 4.
 *
 * Usage:
 *   1. #include "DataManager.h"   (copy from lib/ into the sketch folder)
 *   2. Declare:  DataManager dataMgr;
 *   3. setup():  FIRST drive SD_CS (and TFT_CS) HIGH, then Wire.begin(), then
 *                dataMgr.initSensor();  dataMgr.initSD();
 *   4. loop():   every sample interval:
 *                  if (dataMgr.captureInstantaneousSpectrum())
 *                      dataMgr.processDLIIntegration(dtSeconds);
 *                on hour rollover:  dataMgr.commitHourlyRecordToSD(loc, now);
 *                on day rollover:   dataMgr.rolloverDailyLog(loc, closedDay);
 *                                   dataMgr.resetDLICounter();
 *
 * captureInstantaneousSpectrum() blocks for one integration (~278 ms) and up
 * to four of them if auto-gain has to step.  It returns false if the sensor
 * read failed; the previous values are then left in place.
 *
 * processDLIIntegration() adds the current PAR to the running DLI AND to the
 * current hour's statistics.  Call it once per successful capture.  If the
 * last capture failed it does nothing, so a dropout under-counts rather than
 * re-counting stale light.
 *
 * Output files (created on first write, header row included):
 *   HOURLY.CSV  hour_end,location,par_avg,par_max,dli_so_far,samples,light_type
 *   DAILY.CSV   date,location,dli_mol_m2,par_peak,samples
 * One file for all locations (location is a column) keeps names 8.3-safe.
 *
 * PAR uses the gain the channels were actually read at.  (The standalone
 * sketches use the gain *after* the auto-gain step, which is wrong on the
 * rare cycle where the fourth read still needed a gain change.)
 */

#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <SdFat.h>
#include <Adafruit_AS7341.h>
#include <RTClib.h>

enum LightType : uint8_t {
    LT_SUNLIGHT = 0, LT_FILTERED_DAYLIGHT, LT_COOL_LED, LT_WARM_LED,
    LT_HALOGEN, LT_FLUORESCENT, LT_HPS_HID, LT_UNKNOWN
};

static const char* const LIGHT_NAMES[8] = {
    "Sunlight", "Window Light", "Cool LED", "Warm LED",
    "Halogen",  "Fluorescent",  "HPS/HID",  "Unknown"
};

// Running totals that would be lost on a reset.  Saved periodically by the
// logger (via SettingsManager::saveState) and restored at boot when the
// stamps show it is still the same hour / day.  Stamps are unixtime/3600 and
// unixtime/86400, the same units TimeManager uses for rollover detection.
struct DliState {
    uint32_t dayStamp    = 0;
    uint32_t hourStamp   = 0;
    float    dli         = 0.f;     // mol m-2 since the day began
    float    dayParPeak  = 0.f;
    uint32_t daySamples  = 0;
    float    hourParSum  = 0.f;
    float    hourParMax  = 0.f;
    uint16_t hourSamples = 0;
};

class DataManager {
public:
    static const uint8_t SD_CS_PIN = 4;

    // ---- Init -------------------------------------------------------------

    bool initSensor() {
        if (!sensor.begin()) return false;
        sensor.setATIME(99);
        sensor.setASTEP(999);
        gain = AS7341_GAIN_256X;
        sensor.setGain(gain);
        return true;
    }

    // SHARED_SPI: required to coexist with the Adafruit TFT library.
    // Also loads K_SENSOR from CAL.CFG if present (see hasCalibration()).
    bool initSD() {
        sdOk = sd.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4)));
        calOk = sdOk && loadCalibration();
        return sdOk;
    }

    bool sdAvailable()    const { return sdOk; }
    bool hasCalibration() const { return calOk; }
    float getKSensor()    const { return kSensor; }

    // ---- Sampling ---------------------------------------------------------

    bool captureInstantaneousSpectrum() {
        uint16_t ch[8];
        as7341_gain_t usedGain = gain;

        for (uint8_t attempt = 0; attempt < 4; attempt++) {
            usedGain = gain;
            if (!sensor.readAllChannels()) {
                readFailures++;
                lastCaptureOk = false;
                return false;
            }
            ch[0] = sensor.getChannel(AS7341_CHANNEL_415nm_F1);
            ch[1] = sensor.getChannel(AS7341_CHANNEL_445nm_F2);
            ch[2] = sensor.getChannel(AS7341_CHANNEL_480nm_F3);
            ch[3] = sensor.getChannel(AS7341_CHANNEL_515nm_F4);
            ch[4] = sensor.getChannel(AS7341_CHANNEL_555nm_F5);
            ch[5] = sensor.getChannel(AS7341_CHANNEL_590nm_F6);
            ch[6] = sensor.getChannel(AS7341_CHANNEL_630nm_F7);
            ch[7] = sensor.getChannel(AS7341_CHANNEL_680nm_F8);
            if (!autoAdjustGain(ch)) break;
        }

        uint16_t peak = 0;
        for (uint8_t i = 0; i < 8; i++) {
            rawSpectrum[i] = ch[i];
            if (ch[i] > peak) peak = ch[i];
        }
        readGain   = usedGain;
        saturated  = (peak >= 65000);

        // Gain-normalised peak: counts per unit gain.  Below DARK_NORM_PEAK
        // (~770 counts at 512x) the signal is at the noise floor / leakage
        // level, where the classifier is meaningless (a covered sensor reads
        // as "Halogen" because red and near-IR leak through the cover).
        float gainFactor = GAIN_VALUES[(int)usedGain];
        dark      = ((float)peak / gainFactor) < DARK_NORM_PEAK;
        lightType = dark ? LT_UNKNOWN : classifyLight(ch, usedGain);

        currentPAR = 0.f;
        if (calOk && !dark) {
            float normSum = 0.f;
            for (uint8_t i = 0; i < 8; i++) {
                normSum += (float)ch[i] / gainFactor * CH_BW[i];
            }
            currentPAR = kSensor * normSum;
            if (currentPAR < PAR_FLOOR) currentPAR = 0.f;
        }
        lastCaptureOk = true;
        return true;
    }

    // Adds the current PAR to the running DLI (mol m-2) and the hour stats.
    void processDLIIntegration(float readingDurationSeconds) {
        if (!lastCaptureOk) return;

        dli += currentPAR * readingDurationSeconds * 1e-6f;   // umol -> mol

        hourParSum += currentPAR;
        hourSamples++;
        if (currentPAR > hourParMax) hourParMax = currentPAR;

        daySamples++;
        if (currentPAR > dayParPeak) dayParPeak = currentPAR;
    }

    // ---- Logging ----------------------------------------------------------

    // Appends one row for the hour that just ended, then clears hour stats.
    // The stats are cleared even if the write fails, so one bad write never
    // pollutes the next hour's average.
    bool commitHourlyRecordToSD(const char* locationName, const DateTime& dt) {
        bool ok = false;
        if (sdOk) {
            File32 f = openForAppend("HOURLY.CSV",
                F("hour_end,location,par_avg,par_max,dli_so_far,samples,light_type"));
            if (f) {
                char stamp[24];
                formatDateTime(stamp, sizeof(stamp), dt);
                float avg = (hourSamples > 0) ? hourParSum / hourSamples : 0.f;
                f.print(stamp);             f.print(',');
                f.print(locationName);      f.print(',');
                f.print(avg, 2);            f.print(',');
                f.print(hourParMax, 2);     f.print(',');
                f.print(dli, 4);            f.print(',');
                f.print(hourSamples);       f.print(',');
                f.println(LIGHT_NAMES[(int)lightType]);
                ok = !f.getWriteError();
                f.close();
            }
        }
        if (!ok) writeFailures++;

        hourParSum = 0.f;
        hourParMax = 0.f;
        hourSamples = 0;
        return ok;
    }

    // Appends the daily total.  `day` is the date being CLOSED (when called
    // from a midnight rollover, pass now - TimeSpan(1,0,0,0)).  Does not
    // reset the counters; call resetDLICounter() afterwards.
    bool rolloverDailyLog(const char* locationName, const DateTime& day) {
        bool ok = false;
        if (sdOk) {
            File32 f = openForAppend("DAILY.CSV",
                F("date,location,dli_mol_m2,par_peak,samples"));
            if (f) {
                char date[12];
                snprintf(date, sizeof(date), "%04u-%02u-%02u",
                         (unsigned)day.year(), (unsigned)day.month(), (unsigned)day.day());
                f.print(date);              f.print(',');
                f.print(locationName);      f.print(',');
                f.print(dli, 4);            f.print(',');
                f.print(dayParPeak, 2);     f.print(',');
                f.println(daySamples);
                ok = !f.getWriteError();
                f.close();
            }
        }
        if (!ok) writeFailures++;
        return ok;
    }

    void resetDLICounter() {
        dli = 0.f;
        dayParPeak = 0.f;
        daySamples = 0;
    }

    // ---- Persistence of running totals ------------------------------------

    // Snapshot of the totals, stamped with `now` so a later boot can tell
    // whether they still belong to the current hour / day.
    DliState exportState(const DateTime& now) const {
        DliState s;
        s.dayStamp    = now.unixtime() / 86400UL;
        s.hourStamp   = now.unixtime() / 3600UL;
        s.dli         = dli;
        s.dayParPeak  = dayParPeak;
        s.daySamples  = daySamples;
        s.hourParSum  = hourParSum;
        s.hourParMax  = hourParMax;
        s.hourSamples = hourSamples;
        return s;
    }

    // Overwrites the totals.  The caller decides whether the state is still
    // valid (same location, same day / hour) BEFORE importing.
    void importState(const DliState& s) {
        dli         = s.dli;
        dayParPeak  = s.dayParPeak;
        daySamples  = s.daySamples;
        hourParSum  = s.hourParSum;
        hourParMax  = s.hourParMax;
        hourSamples = s.hourSamples;
    }

    // ---- Accessors --------------------------------------------------------

    const uint16_t* getRawSpectrum() const { return rawSpectrum; }
    float     getCurrentPAR()     const { return currentPAR; }
    float     getCalculatedDLI()  const { return dli; }
    LightType getLightType()      const { return lightType; }
    const char* getLightName()    const { return LIGHT_NAMES[(int)lightType]; }
    float     getGainFactor()     const { return GAIN_VALUES[(int)readGain]; }
    bool      isSaturated()       const { return saturated; }
    bool      isDark()            const { return dark; }   // below noise floor: type Unknown, PAR 0
    uint16_t  getHourSamples()    const { return hourSamples; }

    // Diagnostics for the soak test.
    uint32_t  getReadFailures()   const { return readFailures; }
    uint32_t  getWriteFailures()  const { return writeFailures; }

    // Escape hatch for tools that need the filesystem (e.g. dumping a file).
    SdFat32&  fs() { return sd; }

private:
    Adafruit_AS7341 sensor;
    SdFat32         sd;

    bool  sdOk  = false;
    bool  calOk = false;
    float kSensor = 0.f;

    as7341_gain_t gain     = AS7341_GAIN_256X;   // gain for the NEXT read
    as7341_gain_t readGain = AS7341_GAIN_256X;   // gain the stored spectrum used
    uint16_t  rawSpectrum[8] = {0};
    LightType lightType  = LT_UNKNOWN;
    bool      saturated  = false;
    bool      dark       = false;
    bool      lastCaptureOk = false;
    float     currentPAR = 0.f;

    float    dli        = 0.f;     // mol m-2 since last reset
    float    dayParPeak = 0.f;
    uint32_t daySamples = 0;

    float    hourParSum  = 0.f;
    float    hourParMax  = 0.f;
    uint16_t hourSamples = 0;      // <= 3600 at 1 Hz

    uint32_t readFailures  = 0;
    uint32_t writeFailures = 0;

    // Low-signal thresholds, from covered-sensor readings.  Light leaking
    // through a cover varies with the cover: peaks of ~170 to ~480 counts at
    // 512x (norm 0.33-0.94, PAR 0.06-0.30) were seen.  Genuinely dim indoor
    // light is well above that (peak 1661 at 512x = norm 3.2, PAR 1.4).  The
    // thresholds sit between the two; light below the PAR floor adds
    // essentially nothing to DLI (12 h at 0.5 = 0.02 mol/m2).
    static constexpr float DARK_NORM_PEAK = 1.5f;   // counts / gain (~770 at 512x)
    static constexpr float PAR_FLOOR      = 0.5f;   // umol m-2 s-1

    // Index matches as7341_gain_t (0 = 0.5x ... 10 = 512x).
    static constexpr float GAIN_VALUES[11] = {
        0.5f, 1.f, 2.f, 4.f, 8.f, 16.f, 32.f, 64.f, 128.f, 256.f, 512.f
    };
    // Channel bandwidths (nm FWHM) weighting each channel's photon flux.
    static constexpr float CH_BW[8] = {26.f, 30.f, 36.f, 39.f, 39.f, 40.f, 50.f, 52.f};

    bool autoAdjustGain(const uint16_t* ch) {
        uint16_t peak = 0;
        for (uint8_t i = 0; i < 8; i++) if (ch[i] > peak) peak = ch[i];
        if (peak >= 60000 && gain > AS7341_GAIN_0_5X) {
            gain = (as7341_gain_t)((int)gain - 1);
            sensor.setGain(gain);
            return true;
        }
        if (peak < 1000 && gain < AS7341_GAIN_512X) {
            gain = (as7341_gain_t)((int)gain + 1);
            sensor.setGain(gain);
            return true;
        }
        return false;
    }

    // Thresholds are the tuned values from Spectrometer.ino.
    static LightType classifyLight(const uint16_t* c, as7341_gain_t usedGain) {
        uint32_t total = 0;
        uint16_t minCh = 65535, maxCh = 0;
        for (uint8_t i = 0; i < 8; i++) {
            total += c[i];
            if (c[i] < minCh) minCh = c[i];
            if (c[i] > maxCh) maxCh = c[i];
        }
        if (total == 0 || maxCh == 0) return LT_UNKNOWN;

        float blue    = (float)(c[0] + c[1] + c[2]) / total;
        float green   = (float)(c[3] + c[4])         / total;
        float red     = (float)(c[6] + c[7])         / total;
        float balance = (float)minCh / maxCh;

        // Window glass passes 415 nm, so window light looks like outdoor
        // light; the gain needed to see it is what tells them apart.
        if (balance > 0.15f && blue > 0.15f && red > 0.12f)
            return (usedGain >= AS7341_GAIN_128X) ? LT_FILTERED_DAYLIGHT : LT_SUNLIGHT;
        if (blue > 0.35f) return LT_COOL_LED;
        if (red > 0.30f) {
            float ir = (c[6] > 0) ? (float)c[7] / c[6] : 0.f;
            return (ir > 0.75f) ? LT_HALOGEN : LT_WARM_LED;
        }
        if (green > 0.40f) return LT_HPS_HID;
        float yellow = (float)c[5] / total;
        if ((green + yellow) > 0.42f && blue > 0.14f && blue < 0.32f) return LT_FLUORESCENT;
        return LT_UNKNOWN;
    }

    // Reads K_SENSOR=<float> from CAL.CFG (written by Calibration.ino).
    bool loadCalibration() {
        File32 f = sd.open("CAL.CFG", O_READ);
        if (!f) return false;

        char line[40];
        bool found = false;
        int n;
        while ((n = f.fgets(line, sizeof(line))) > 0) {
            if (strncmp(line, "K_SENSOR=", 9) == 0) {
                kSensor = atof(line + 9);
                found = (kSensor > 0.f);
                break;
            }
        }
        f.close();
        return found;
    }

    File32 openForAppend(const char* name, const __FlashStringHelper* header) {
        bool isNew = !sd.exists(name);
        File32 f = sd.open(name, O_WRITE | O_CREAT | O_APPEND);
        if (f && isNew) f.println(header);
        return f;
    }

    static void formatDateTime(char* buf, size_t len, const DateTime& t) {
        snprintf(buf, len, "%04u-%02u-%02u %02u:%02u:%02u",
                 (unsigned)t.year(), (unsigned)t.month(), (unsigned)t.day(),
                 (unsigned)t.hour(), (unsigned)t.minute(), (unsigned)t.second());
    }
};

constexpr float DataManager::DARK_NORM_PEAK;
constexpr float DataManager::PAR_FLOOR;
constexpr float DataManager::GAIN_VALUES[11];
constexpr float DataManager::CH_BW[8];
