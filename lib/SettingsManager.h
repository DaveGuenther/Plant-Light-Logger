/**
 * SettingsManager.h — header-only library
 * Plant Light Logger project
 *
 * Persists the logger's user settings on the SD card:
 *   - the list of named locations (up to MAX_LOCATIONS) and which is active
 *   - the logging interval
 *   - a snapshot of the running DLI totals, so a reset or power loss does
 *     not lose the day (see DliState in DataManager.h)
 *
 * Files (8.3 names, KEY=value text like CAL.CFG):
 *   SETTINGS.CFG   ACTIVE_LOCATION=<name>   LOG_INTERVAL_MIN=<minutes>
 *   LOCS.CFG       one location name per line
 *   STATE.CFG      DliState fields plus the location they belong to
 *
 * Every write goes to a .TMP file first, then replaces the real file, so a
 * power cut mid-save cannot leave a half-written file.  If the cut lands
 * between the delete and the rename, the .TMP is used at the next load.
 *
 * Usage:
 *   settings.begin(&dataMgr.fs());       // after dataMgr.initSD()
 *   const char* loc = settings.getActiveLocation();
 *   settings.addLocation("Office");       // saved immediately
 *   settings.setActiveLocation(2);        // saved immediately
 *
 * begin() never fails fatally: with no card, or no files, it falls back to
 * one location called "DEFAULT_LOC" and a 60 minute interval.  Saves then
 * fail quietly and are counted in getSaveFailures().
 *
 * Switching location is a data boundary: before calling setActiveLocation(),
 * the logger should write the current hour and day records under the OLD
 * location and reset the counters (DataManager already has the calls).
 *
 * Names: 1-15 chars from letters, digits, space, '_', '-', '.'; no leading
 * or trailing space; unique ignoring case.  Those characters are safe inside
 * the CSV files and the KEY=value files.
 */

#pragma once

#include <Arduino.h>
#include <SdFat.h>
#include "DataManager.h"     // DliState

enum class LocResult : uint8_t {
    OK, INVALID_NAME, DUPLICATE, FULL, LAST_LOCATION, BAD_INDEX
};

class SettingsManager {
public:
    static const uint8_t MAX_LOCATIONS = 10;
    static const uint8_t NAME_MAX      = 15;

    // `filesystem` may be nullptr when the SD card is down.
    // Returns true if settings were read from the card, false if defaults
    // are in use (no card, or first run).
    bool begin(SdFat32* filesystem) {
        fs = filesystem;
        bool gotLocs = loadLocations();
        bool gotCfg  = loadSettings();
        fromCard = gotLocs && gotCfg;
        return fromCard;
    }

    // ---- Locations --------------------------------------------------------

    uint8_t     getLocationCount() const { return count; }
    uint8_t     getActiveIndex()   const { return activeIdx; }
    const char* getActiveLocation() const { return names[activeIdx]; }
    const char* getLocation(uint8_t i) const { return (i < count) ? names[i] : ""; }

    int8_t findLocation(const char* name) const {
        for (uint8_t i = 0; i < count; i++) {
            if (strcasecmp(names[i], name) == 0) return (int8_t)i;
        }
        return -1;
    }

    LocResult addLocation(const char* name) {
        if (!validName(name))       return LocResult::INVALID_NAME;
        if (findLocation(name) >= 0) return LocResult::DUPLICATE;
        if (count >= MAX_LOCATIONS) return LocResult::FULL;
        strncpy(names[count], name, NAME_MAX);
        names[count][NAME_MAX] = '\0';
        count++;
        saveLocations();
        return LocResult::OK;
    }

    // Removes a name from the list only; logged data is never touched.
    // The last remaining location cannot be removed.
    LocResult removeLocation(uint8_t i) {
        if (i >= count)  return LocResult::BAD_INDEX;
        if (count == 1)  return LocResult::LAST_LOCATION;
        for (uint8_t k = i; k + 1 < count; k++) {
            memcpy(names[k], names[k + 1], NAME_MAX + 1);
        }
        count--;
        if (i == activeIdx)    activeIdx = 0;
        else if (i < activeIdx) activeIdx--;
        saveLocations();
        saveSettings();
        return LocResult::OK;
    }

    LocResult setActiveLocation(uint8_t i) {
        if (i >= count) return LocResult::BAD_INDEX;
        activeIdx = i;
        saveSettings();
        return LocResult::OK;
    }

    // ---- Logging interval -------------------------------------------------

    uint16_t getLogIntervalMin() const { return intervalMin; }

    // Allowed: 5, 10, 15, 30, 60.  Returns false for anything else.
    bool setLogIntervalMin(uint16_t minutes) {
        if (!validInterval(minutes)) return false;
        intervalMin = minutes;
        saveSettings();
        return true;
    }

    // ---- Running-total snapshot -------------------------------------------

    bool saveState(const DliState& s, const char* location) {
        if (!fs) return fail();
        File32 f = beginWrite(STATE_TMP);
        if (!f) return fail();
        f.print(F("LOCATION="));     f.println(location);
        f.print(F("DAY_STAMP="));    f.println(s.dayStamp);
        f.print(F("HOUR_STAMP="));   f.println(s.hourStamp);
        f.print(F("DLI="));          f.println(s.dli, 6);
        f.print(F("DAY_PEAK="));     f.println(s.dayParPeak, 4);
        f.print(F("DAY_SAMPLES="));  f.println(s.daySamples);
        f.print(F("HOUR_SUM="));     f.println(s.hourParSum, 4);
        f.print(F("HOUR_MAX="));     f.println(s.hourParMax, 4);
        f.print(F("HOUR_SAMPLES=")); f.println(s.hourSamples);
        return commit(f, STATE_FILE, STATE_TMP) || fail();
    }

    // `locationOut` must hold NAME_MAX + 1 chars.  Returns false if there is
    // no state file or it is incomplete / corrupt.
    bool loadState(DliState& s, char* locationOut) {
        if (!fs) return false;
        File32 f;
        if (!openCfg(STATE_FILE, STATE_TMP, f)) return false;

        DliState t;
        char loc[NAME_MAX + 1] = {0};
        uint16_t seen = 0;
        char line[48];
        while (f.fgets(line, sizeof(line)) > 0) {
            stripEol(line);
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = '\0';
            const char* key = line;
            const char* val = eq + 1;

            if      (!strcmp(key, "LOCATION"))     { strncpy(loc, val, NAME_MAX); seen |= 1; }
            else if (!strcmp(key, "DAY_STAMP"))    { t.dayStamp    = strtoul(val, NULL, 10); seen |= 2; }
            else if (!strcmp(key, "HOUR_STAMP"))   { t.hourStamp   = strtoul(val, NULL, 10); seen |= 4; }
            else if (!strcmp(key, "DLI"))          { t.dli         = atof(val); seen |= 8; }
            else if (!strcmp(key, "DAY_PEAK"))     { t.dayParPeak  = atof(val); seen |= 16; }
            else if (!strcmp(key, "DAY_SAMPLES"))  { t.daySamples  = strtoul(val, NULL, 10); seen |= 32; }
            else if (!strcmp(key, "HOUR_SUM"))     { t.hourParSum  = atof(val); seen |= 64; }
            else if (!strcmp(key, "HOUR_MAX"))     { t.hourParMax  = atof(val); seen |= 128; }
            else if (!strcmp(key, "HOUR_SAMPLES")) { t.hourSamples = (uint16_t)strtoul(val, NULL, 10); seen |= 256; }
        }
        f.close();

        if (seen != 0x1FF) return false;      // all nine keys required
        s = t;
        strncpy(locationOut, loc, NAME_MAX);
        locationOut[NAME_MAX] = '\0';
        return true;
    }

    void clearState() {
        if (!fs) return;
        fs->remove(STATE_FILE);
        fs->remove(STATE_TMP);
    }

    // ---- Diagnostics ------------------------------------------------------

    bool     loadedFromCard()  const { return fromCard; }
    uint32_t getSaveFailures() const { return saveFailures; }

    // Wipes all three files (used by tests; the next begin() uses defaults).
    void eraseAll() {
        if (!fs) return;
        const char* files[] = { SETTINGS_FILE, SETTINGS_TMP, LOCS_FILE, LOCS_TMP,
                                STATE_FILE, STATE_TMP };
        for (uint8_t i = 0; i < 6; i++) fs->remove(files[i]);
    }

private:
    static constexpr const char* SETTINGS_FILE = "SETTINGS.CFG";
    static constexpr const char* SETTINGS_TMP  = "SETTINGS.TMP";
    static constexpr const char* LOCS_FILE     = "LOCS.CFG";
    static constexpr const char* LOCS_TMP      = "LOCS.TMP";
    static constexpr const char* STATE_FILE    = "STATE.CFG";
    static constexpr const char* STATE_TMP     = "STATE.TMP";

    SdFat32* fs = nullptr;

    char     names[MAX_LOCATIONS][NAME_MAX + 1];
    uint8_t  count       = 0;
    uint8_t  activeIdx   = 0;
    uint16_t intervalMin = 60;
    bool     fromCard    = false;
    uint32_t saveFailures = 0;

    // ---- Validation / text helpers ----

    static bool validName(const char* s) {
        size_t len = strlen(s);
        if (len < 1 || len > NAME_MAX) return false;
        if (s[0] == ' ' || s[len - 1] == ' ') return false;
        for (size_t i = 0; i < len; i++) {
            char c = s[i];
            bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == ' ' || c == '_' ||
                      c == '-' || c == '.';
            if (!ok) return false;
        }
        return true;
    }

    static bool validInterval(uint16_t m) {
        return m == 5 || m == 10 || m == 15 || m == 30 || m == 60;
    }

    static void stripEol(char* s) {
        size_t n = strlen(s);
        while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
    }

    bool fail() { saveFailures++; return false; }

    // ---- File helpers ----

    // Opens the real file, or its .TMP if a save was interrupted.
    bool openCfg(const char* name, const char* tmp, File32& f) {
        f = fs->open(name, O_READ);
        if (f) return true;
        f = fs->open(tmp, O_READ);
        return (bool)f;
    }

    File32 beginWrite(const char* tmp) {
        fs->remove(tmp);
        return fs->open(tmp, O_WRITE | O_CREAT | O_TRUNC);
    }

    // Closes `f` and swaps the .TMP in for the real file.
    bool commit(File32& f, const char* name, const char* tmp) {
        bool ok = !f.getWriteError();
        ok = f.close() && ok;
        if (!ok) return false;
        fs->remove(name);
        return fs->rename(tmp, name);
    }

    // ---- Load / save ----

    void setDefaults() {
        strncpy(names[0], "DEFAULT_LOC", NAME_MAX);
        names[0][NAME_MAX] = '\0';
        count     = 1;
        activeIdx = 0;
    }

    bool loadLocations() {
        count = 0;
        File32 f;
        bool found = fs && openCfg(LOCS_FILE, LOCS_TMP, f);
        if (found) {
            char line[NAME_MAX + 8];
            while (count < MAX_LOCATIONS && f.fgets(line, sizeof(line)) > 0) {
                stripEol(line);
                if (!validName(line) || findLocation(line) >= 0) continue;
                strncpy(names[count], line, NAME_MAX);
                names[count][NAME_MAX] = '\0';
                count++;
            }
            f.close();
        }
        if (count == 0) { setDefaults(); return false; }
        return true;
    }

    bool loadSettings() {
        activeIdx   = 0;
        intervalMin = 60;
        File32 f;
        if (!fs || !openCfg(SETTINGS_FILE, SETTINGS_TMP, f)) return false;

        char line[NAME_MAX + 24];
        while (f.fgets(line, sizeof(line)) > 0) {
            stripEol(line);
            char* eq = strchr(line, '=');
            if (!eq) continue;                       // also skips '#' comments
            *eq = '\0';
            const char* key = line;
            const char* val = eq + 1;
            if (!strcmp(key, "ACTIVE_LOCATION")) {
                int8_t i = findLocation(val);
                if (i >= 0) activeIdx = (uint8_t)i;
            } else if (!strcmp(key, "LOG_INTERVAL_MIN")) {
                uint16_t m = (uint16_t)atoi(val);
                if (validInterval(m)) intervalMin = m;
            }
        }
        f.close();
        return true;
    }

    bool saveLocations() {
        if (!fs) return fail();
        File32 f = beginWrite(LOCS_TMP);
        if (!f) return fail();
        for (uint8_t i = 0; i < count; i++) f.println(names[i]);
        return commit(f, LOCS_FILE, LOCS_TMP) || fail();
    }

    bool saveSettings() {
        if (!fs) return fail();
        File32 f = beginWrite(SETTINGS_TMP);
        if (!f) return fail();
        f.println(F("# Plant Light Logger settings"));
        f.print(F("ACTIVE_LOCATION=")); f.println(names[activeIdx]);
        f.print(F("LOG_INTERVAL_MIN=")); f.println(intervalMin);
        return commit(f, SETTINGS_FILE, SETTINGS_TMP) || fail();
    }
};

constexpr const char* SettingsManager::SETTINGS_FILE;
constexpr const char* SettingsManager::SETTINGS_TMP;
constexpr const char* SettingsManager::LOCS_FILE;
constexpr const char* SettingsManager::LOCS_TMP;
constexpr const char* SettingsManager::STATE_FILE;
constexpr const char* SettingsManager::STATE_TMP;
