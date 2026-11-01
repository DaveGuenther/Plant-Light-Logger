/**
 * SettingsManagerTest.ino
 * Exercises SettingsManager (locations, active location, logging interval,
 * and the running-DLI snapshot) over the Serial Monitor.  Only the SD card
 * is needed; no sensor, RTC or display.
 *
 * Wiring (Mega 2560):
 *   SD card  CS -> 4   MOSI -> 51   MISO -> 50   SCK -> 52
 *   TFT, if still connected, has its CS (pin 7) driven HIGH below.
 *
 * Serial Monitor: 115200 baud, line ending "Newline".
 *
 * Commands:
 *   l            list locations, active one, interval, save failures
 *   a NAME       add a location, e.g.  a Office
 *   r N          remove location number N (from the list)
 *   u N          make location number N active
 *   i MINUTES    set logging interval (5, 10, 15, 30, 60)
 *   S            save a made-up DLI snapshot under the active location
 *   L            load the snapshot back and print it (compare with S)
 *   c            clear the snapshot
 *   f            print SETTINGS.CFG, LOCS.CFG and STATE.CFG
 *   x            erase all three files (next boot uses defaults)
 *   ?            help
 *
 * Persistence test: add a location, make it active, press the Mega's reset
 * button (or unplug and replug), then 'l' - the list and active location
 * should have survived.
 *
 * Libraries: Adafruit AS7341, RTClib, SdFat (DataManager.h pulls them in)
 *
 * NOTE: copy SettingsManager.h AND DataManager.h from lib/ into this sketch
 *       folder before compiling.
 */

#include <Wire.h>
#include <SPI.h>
#include "DataManager.h"
#include "SettingsManager.h"

#define SD_CS    4
#define TFT_CS   7

DataManager     dataMgr;       // used here only to bring up the SD card
SettingsManager settings;

static const char* resultName(LocResult r) {
    switch (r) {
        case LocResult::OK:            return "OK";
        case LocResult::INVALID_NAME:  return "INVALID NAME (1-15 chars: letters, digits, space _ - .)";
        case LocResult::DUPLICATE:     return "DUPLICATE";
        case LocResult::FULL:          return "LIST FULL";
        case LocResult::LAST_LOCATION: return "CAN'T REMOVE THE LAST LOCATION";
        case LocResult::BAD_INDEX:     return "BAD NUMBER";
    }
    return "?";
}

static void listAll() {
    Serial.println(F("Locations:"));
    for (uint8_t i = 0; i < settings.getLocationCount(); i++) {
        Serial.print(i == settings.getActiveIndex() ? F("  * ") : F("    "));
        Serial.print(i);
        Serial.print(F("  "));
        Serial.println(settings.getLocation(i));
    }
    Serial.print(F("Interval: ")); Serial.print(settings.getLogIntervalMin());
    Serial.println(F(" min"));
    Serial.print(F("Loaded from card: ")); Serial.print(settings.loadedFromCard() ? F("yes") : F("no (defaults)"));
    Serial.print(F("   save failures: ")); Serial.println(settings.getSaveFailures());
}

static void printState(const DliState& s) {
    Serial.print(F("  dayStamp="));    Serial.println(s.dayStamp);
    Serial.print(F("  hourStamp="));   Serial.println(s.hourStamp);
    Serial.print(F("  dli="));         Serial.println(s.dli, 6);
    Serial.print(F("  dayParPeak="));  Serial.println(s.dayParPeak, 4);
    Serial.print(F("  daySamples="));  Serial.println(s.daySamples);
    Serial.print(F("  hourParSum="));  Serial.println(s.hourParSum, 4);
    Serial.print(F("  hourParMax="));  Serial.println(s.hourParMax, 4);
    Serial.print(F("  hourSamples=")); Serial.println(s.hourSamples);
}

static void dumpFile(const char* name) {
    Serial.print(F("--- ")); Serial.println(name);
    File32 f = dataMgr.fs().open(name, O_READ);
    if (!f) { Serial.println(F("(not found)")); return; }
    while (f.available()) Serial.write(f.read());
    f.close();
}

static void printHelp() {
    Serial.println(F("l list | a NAME add | r N remove | u N use | i MIN interval"));
    Serial.println(F("S save snapshot | L load snapshot | c clear snapshot | f dump files | x erase all"));
}

static void handleCommand(char* line) {
    char cmd = line[0];
    char* arg = line + 1;
    while (*arg == ' ') arg++;

    switch (cmd) {
        case 'l': listAll(); break;

        case 'a':
            Serial.println(resultName(settings.addLocation(arg)));
            break;

        case 'r':
            Serial.println(resultName(settings.removeLocation((uint8_t)atoi(arg))));
            break;

        case 'u':
            Serial.println(resultName(settings.setActiveLocation((uint8_t)atoi(arg))));
            break;

        case 'i':
            Serial.println(settings.setLogIntervalMin((uint16_t)atoi(arg))
                           ? F("OK") : F("INVALID (use 5, 10, 15, 30 or 60)"));
            break;

        case 'S': {
            DliState s;
            s.dayStamp    = 20393;       // arbitrary but recognisable
            s.hourStamp   = 489456;
            s.dli         = 1.234567f;
            s.dayParPeak  = 456.7891f;
            s.daySamples  = 41234;
            s.hourParSum  = 98765.4321f;
            s.hourParMax  = 321.5f;
            s.hourSamples = 1800;
            Serial.println(settings.saveState(s, settings.getActiveLocation())
                           ? F("Snapshot saved") : F("Snapshot save FAILED"));
            printState(s);
            break;
        }

        case 'L': {
            DliState s;
            char loc[SettingsManager::NAME_MAX + 1];
            if (settings.loadState(s, loc)) {
                Serial.print(F("Snapshot loaded for location: ")); Serial.println(loc);
                printState(s);
            } else {
                Serial.println(F("No valid snapshot"));
            }
            break;
        }

        case 'c':
            settings.clearState();
            Serial.println(F("Snapshot cleared"));
            break;

        case 'f':
            dumpFile("SETTINGS.CFG");
            dumpFile("LOCS.CFG");
            dumpFile("STATE.CFG");
            break;

        case 'x':
            settings.eraseAll();
            Serial.println(F("Files erased - reset the board to see defaults"));
            break;

        case '?': printHelp(); break;
        default: break;
    }
}

void setup() {
    Serial.begin(115200);

    pinMode(SD_CS, OUTPUT);   digitalWrite(SD_CS, HIGH);
    pinMode(TFT_CS, OUTPUT);  digitalWrite(TFT_CS, HIGH);

    Serial.println(F("--- SettingsManager test ---"));
    bool sdOk = dataMgr.initSD();
    Serial.print(F("SD: ")); Serial.println(sdOk ? F("OK") : F("INIT FAILED (defaults only, saves will fail)"));

    settings.begin(sdOk ? &dataMgr.fs() : nullptr);
    listAll();
    printHelp();
}

void loop() {
    static char line[40];
    static uint8_t pos = 0;

    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            if (pos > 0) {
                line[pos] = '\0';
                handleCommand(line);
                pos = 0;
            }
        } else if (pos < sizeof(line) - 1) {
            line[pos++] = c;
        }
    }
}
