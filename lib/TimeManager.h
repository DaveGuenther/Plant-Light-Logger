/**
 * TimeManager.h — header-only library
 * Plant Light Logger project
 *
 * Wraps the DS3231 RTC (RTClib) and latches hour / day rollover events so the
 * logger can write its hourly and daily records without polling the RTC
 * itself.
 *
 * Hardware: DS3231 on the shared I2C bus (SDA 20, SCL 21), address 0x68.
 *
 * Usage:
 *   1. #include "TimeManager.h"   (copy from lib/ into the sketch folder)
 *   2. Declare:  TimeManager timeMgr;
 *   3. setup():  if (!timeMgr.init()) { ... RTC not found ... }
 *   4. loop():   DateTime now = timeMgr.getCurrentTime();
 *                if (timeMgr.hasHourRolledOver()) { ... }
 *                if (timeMgr.hasDayRolledOver())  { ... }
 *
 * hasHourRolledOver() / hasDayRolledOver() — each returns true ONCE per
 *   rollover (cleared on read).  The two flags are independent, so reading
 *   one never consumes the other.  At midnight both fire.
 *
 * setCurrentTime() — re-baselines the rollover tracking, so changing the
 *   clock never produces a false event.  Events only fire for boundaries
 *   crossed afterwards.
 *
 * wasClockReset() — true if init() found the RTC had lost power (dead or
 *   missing coin cell) and set it to the sketch compile time.  The time is
 *   then only approximate; the UI should prompt the user to set the clock.
 *
 * The RTC is read over I2C at most every POLL_MS, however often the
 * rollover checks are called.
 */

#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>

class TimeManager {
public:
    bool init() {
        if (!rtc.begin()) return false;
        clockReset = rtc.lostPower();
        if (clockReset) {
            rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));   // compile time
        }
        rebaseline(rtc.now());
        return true;
    }

    DateTime getCurrentTime() { return rtc.now(); }

    void setCurrentTime(uint16_t year, uint8_t month, uint8_t day,
                        uint8_t hour, uint8_t min, uint8_t sec) {
        rtc.adjust(DateTime(year, month, day, hour, min, sec));
        clockReset = false;
        rebaseline(rtc.now());
    }

    bool hasHourRolledOver() { poll(); return takeFlag(hourFlag); }
    bool hasDayRolledOver()  { poll(); return takeFlag(dayFlag); }

    bool wasClockReset() const { return clockReset; }

private:
    static const unsigned long POLL_MS = 250;

    RTC_DS3231    rtc;
    uint32_t      lastHourStamp = 0;   // unixtime / 3600
    uint32_t      lastDayStamp  = 0;   // unixtime / 86400
    bool          hourFlag      = false;
    bool          dayFlag       = false;
    bool          clockReset    = false;
    unsigned long lastPollMs    = 0;

    void rebaseline(const DateTime& now) {
        lastHourStamp = now.unixtime() / 3600UL;
        lastDayStamp  = now.unixtime() / 86400UL;
        hourFlag = dayFlag = false;
        lastPollMs = millis();
    }

    void poll() {
        unsigned long ms = millis();
        if (ms - lastPollMs < POLL_MS) return;
        lastPollMs = ms;

        uint32_t t = rtc.now().unixtime();
        uint32_t h = t / 3600UL;
        uint32_t d = t / 86400UL;
        if (h != lastHourStamp) { lastHourStamp = h; hourFlag = true; }
        if (d != lastDayStamp)  { lastDayStamp  = d; dayFlag  = true; }
    }

    static bool takeFlag(bool& flag) {
        bool was = flag;
        flag = false;
        return was;
    }
};
