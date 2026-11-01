# Comprehensive Software & Hardware Specification: Daily Light Integrator (DLI) Logger

This document serves as the master specification for building a custom, highly modular, object-oriented **Daily Light Integrator (DLI) Logger & Spectrometer** using an **ATmega2560** microcontroller. It is structured explicitly for an LLM/Coder Agent to implement, compile, and unit-test the firmware in a modular, step-by-step fashion.

---

## 1. System Architecture Overview

The system runs on an ATmega2560, leveraging its ample flash memory (256 KB) and GPIO count to implement a rigorous **Object-Oriented Design (OOD)** following an adapted **Model-View-Controller (MVC)** separation:
* **Hardware Wrappers (Drivers/Managers):** Encapsulate low-level communication protocols (I2C, SPI, GPIO Pullups) and expose clean, abstract APIs to the rest of the software.
* **Data Models / Domain Logic:** Track time records, accumulate spectral data, calculate Photosynthetically Active Radiation (PAR), integrate readings over a rolling 24-hour window to calculate DLI, and handle non-volatile CSV file writing (SD card).
* **UI Engine (State Pattern):** Manages user interactions via a hierarchical screen layout driven by an analog joystick and 4 tactile buttons.

### High-Level Subsystems
1. **Input Subsystem (`InputManager`):** Debounces tactile switches and normalizes an analog thumb joystick into digital directional ticks (`UP`, `DOWN`, `LEFT`, `RIGHT`).
2. **Time Subsystem (`TimeManager`):** Communicates with a DS3231 I2C RTC to keep precise scheduling, tracking hourly transitions and midnight rollovers.
3. **Core Analytics & Storage Subsystem (`DataManager`):** Controls an AS7341 11-channel spectral sensor, applies calibration matrices to derive PAR, integrates readings over a rolling 24-hour window to calculate DLI, and stores data logs on an SD card.
4. **UI State Machine (`UIManager` & Views):** Renders a 1.8" TFT display dynamically without using full-screen clearing (`fillScreen`) to eliminate refresh flicker.

---

## 2. Complete Hardware Mapping & Pin Configuration

The ATmega2560 has unique hardware peripheral configurations. Pins must be wired exactly as specified below. Sharing the I2C bus is permitted, while SPI devices share data lines but utilize unique Chip Select (CS) controls.

### 2.1 Shared I2C Bus (Pins 20, 21)
* **SDA:** Pin 20 (Shared between DS3231 RTC and AS7341 Spectral Sensor)
* **SCL:** Pin 21 (Shared between DS3231 RTC and AS7341 Spectral Sensor)
* **Power:** +5V Rail, Common GND Rail
* **Level shifting:** None needed. I2C is open-drain — devices only pull lines LOW, and the HIGH level comes from each module's onboard pull-up resistors, which work fine at 5V.
* **Address check:** DS3231 = `0x68`, AS7341 = `0x39` — no conflict.

### 2.2 Shared SPI Bus (Pins 50, 51, 52)
* **MISO:** Pin 50 (Master In Slave Out - Connected to SD Card Slot out)
* **MOSI (DIN):** Pin 51 (Master Out Slave In - Connected to TFT DIN and SD Card MOSI)
* **SCK (CLK):** Pin 52 (Serial Clock - Connected to TFT CLK and SD Card CLK)

### 2.3 Individual Control/Select Pins

The TFT/SD module has 15 pins. Pin numbers below refer to the physical module header (15-pin side). Signal lines driven by the 5V Mega require 330Ω/680Ω voltage dividers; lines that carry 3.3V signals from the module back to the Mega do not.

| Component | Module Pin | Mega 2560 Pin | Divider? | Description |
| :--- | :--- | :--- | :--- | :--- |
| **1.8" ST7735 TFT** | Pin 3 — RST | D5 | Yes (330/680Ω) | Hardware Reset |
| **1.8" ST7735 TFT** | Pin 4 — RS/DC | D6 | Yes (330/680Ω) | Data / Command Toggle |
| **1.8" ST7735 TFT** | Pin 5 — SDA/MOSI | 51 (HW SPI) | Yes (330/680Ω) | Shared SPI data line |
| **1.8" ST7735 TFT** | Pin 6 — SCK | 52 (HW SPI) | Yes (330/680Ω) | Shared SPI clock |
| **1.8" ST7735 TFT** | Pin 7 — CS | D7 | Yes (330/680Ω) | TFT Chip Select |
| **1.8" ST7735 TFT** | Pin 8 — BL | D44 (PWM) | No (current-limiting only, ~1.4mA via 68Ω) | Backlight power |
| **MicroSD Card Slot** | Pin 11 — SD_CS | D4 | Yes (330/680Ω) | SD Chip Select |
| **MicroSD Card Slot** | Pin 12 — SD_MOSI | 51 (shared) | Yes (330/680Ω) | Shared SPI data line |
| **MicroSD Card Slot** | Pin 13 — SD_MISO | 50 (HW SPI) | **No** (3.3V out) | SD → Mega; no divider needed |
| **MicroSD Card Slot** | Pin 14 — SD_CLK | 52 (shared) | Yes (330/680Ω) | Shared SPI clock |
| **Tactile Button SELECT** | Switch Terminal 1 | Pin 22 | No (INPUT_PULLUP) | Reads LOW when pressed |
| **Tactile Button BACK** | Switch Terminal 1 | Pin 23 | No (INPUT_PULLUP) | Reads LOW when pressed |
| **Tactile Button VIEW** | Switch Terminal 1 | Pin 24 | No (INPUT_PULLUP) | Reads LOW when pressed |
| **Tactile Button SETTINGS** | Switch Terminal 1 | Pin 25 | No (INPUT_PULLUP) | Reads LOW when pressed |
| **Thumb Joystick X** | VRx (Horizontal) | Pin A0 | No | Analog: Left/Right Position |
| **Thumb Joystick Y** | VRy (Vertical) | Pin A1 | No | Analog: Up/Down Position |

*Module power — Pin 1 → GND, Pin 2 → 3.3V (jumper J1 SHORT for 3.3V mode) or 5V (jumper J1 OPEN for 5V mode). GND pins of the joystick potentiometers, sensor, RTC, TFT, and SD reader must connect directly to the common ground rail. Tactile button wiring is covered in section 2.4 below.*

### 2.4 Tactile Button & Potentiometer Wiring

#### Tactile Buttons (Pins 22–25)

Each button has two terminals. Wire one terminal to the Mega pin and the other terminal to GND. Nothing else — no resistors, no connection to 5V.

```
Mega Pin 22 ──── [terminal 1]
                      │
                 [ button ]
                      │
                 [terminal 2] ──── GND
```

The firmware configures each pin as `INPUT_PULLUP`. This activates an internal ~50kΩ resistor that holds the pin at 5V (HIGH) when the button is open. Pressing the button connects the pin directly to GND, pulling it LOW. The firmware treats LOW as "pressed". No external pull-up or pull-down resistor is needed.

#### Joystick Potentiometers (Pins A0, A1)

Each potentiometer axis has three terminals: two outer terminals and a middle wiper terminal. Wire them as a voltage divider:

```
5V  ──── [outer terminal A]
               │
          [wiper / middle] ──── A0 (X axis) or A1 (Y axis)
               │
GND ──── [outer terminal B]
```

With 5V across the pot, the wiper sweeps from near-0V to near-5V as the joystick moves, which the ADC reads as 0–1023. At rest/centre the reading will be approximately 512. The firmware treats readings below 200 as one extreme and above 823 as the other.

**Direction note:** it does not matter which outer terminal goes to 5V vs GND, but the choice determines which physical direction reads high vs low. If the joystick direction appears inverted, swap the outer terminal connections (GND ↔ 5V) for that axis rather than changing the code.

### 2.5 TFT Display Library & Level Shifting

**Library: Adafruit ST7735 and ST7789 Library + Adafruit GFX Library** (install both via Library Manager). The constructor takes the CS, DC, and RST pins directly — no external configuration file required:

```cpp
#define TFT_CS   7
#define TFT_RST  5
#define TFT_DC   6
#define TFT_BL   44  // backlight, via 68Ω resistor

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
```

Initialise with `tft.initR(INITR_BLACKTAB)` then `tft.setRotation(1)` for landscape, after setting the backlight pin HIGH:

```cpp
pinMode(TFT_BL, OUTPUT);
digitalWrite(TFT_BL, HIGH);  // backlight on
```

Colour constants use the `ST77XX_` prefix (e.g. `ST77XX_BLACK`, `ST77XX_WHITE`, `ST77XX_RED`). The backlight (module pin 8) is wired to Mega D44 through a 68Ω current-limiting resistor — D44 is PWM-capable for future dimming, but is simply driven HIGH for full brightness here.

**Level shifting (5V Mega → 3.3V module).** Nine signal lines total need 330Ω/680Ω voltage dividers (see table in 2.3): TFT RST, DC, MOSI, SCK, CS, plus SD CS, SD MOSI, and SD CLK. SD MISO (pin 50) and BL do not need dividers — MISO is a 3.3V output from the card, and BL is a power line through a current-limiting resistor, not a logic signal.

Each voltage divider per signal line:

```
Mega pin ──[ 330Ω ]──┬── Module pin
                     │
                   [680Ω]
                     │
                    GND
Output = 5V × 680/(330+680) ≈ 3.37V  ✓
```

> **Do not use high-value resistors** (e.g. 3kΩ/5.6kΩ). The resulting ~2kΩ Thevenin impedance forms an RC low-pass filter with wire capacitance that rounds SPI edges faster than the bit period, corrupting all pixel writes even though the slower init sequence appears to succeed (symptom: `fillScreen` has no effect).

### 2.6 RTC Library (DS3231)

**Library: RTClib by Adafruit** (install via Library Manager; accept the Adafruit BusIO dependency prompt). This is the library `TimeManager` (section 3.1) wraps.

```cpp
#include <Wire.h>
#include <RTClib.h>

RTC_DS3231 rtc;
```

In `setup()`:
```cpp
Wire.begin();
if (!rtc.begin()) {
    // RTC not found — check wiring
}
if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));  // set to compile time
}
```

`rtc.lostPower()` returns true only when the backup coin cell is dead or missing, so the time-set call won't run on every boot.

---

## 3. Object-Oriented Software Design Spec

To maximize architectural maintainability, all domain spaces are modeled inside discrete classes. 

### 3.1 Class Diagrams & Interfaces

#### InputManager
Enforces button debouncing using non-blocking software timers and converts analog joystick sweeps into precise discrete movements using strict window thresholds.
```cpp
enum class ButtonId { SELECT, BACK, VIEW, SETTINGS };
enum class JoystickDir { NONE, UP, DOWN, LEFT, RIGHT };

class InputManager {
private:
    const int btnPins[4] = {22, 23, 24, 25};
    bool lastBtnState[4];
    unsigned long lastDebounceTime[4];
    const unsigned long debounceDelay = 35; // ms

    const int pinX = A0;
    const int pinY = A1;
    const int thresholdLow = 200;
    const int thresholdHigh = 823;
    JoystickDir lastJoyState = JoystickDir::NONE;
    unsigned long lastJoyTick = 0;
    const unsigned long joyRepeatDelay = 250; // ms

public:
    void init();
    void update(); // Call at the start of loop()
    bool isButtonPressed(ButtonId btn);
    JoystickDir getJoystickDirection();
};
```

#### TimeManager
**Implemented:** [`lib/TimeManager.h`](../lib/TimeManager.h) (header-only; tested with `stand-alone-scripts/TimeManagerTest`).

Wraps the Adafruit `RTClib` and latches hour / calendar-day rollover events.
```cpp
#include <RTClib.h>

class TimeManager {
public:
    bool init();              // false if the DS3231 is not found
    DateTime getCurrentTime();
    void setCurrentTime(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t min, uint8_t sec);
    bool hasHourRolledOver(); // true ONCE per rollover (cleared on read)
    bool hasDayRolledOver();  // independent of the hour flag; both fire at midnight
    bool wasClockReset() const; // RTC lost power at init and was set to compile time
};
```
Design notes:
* Rollovers are detected by comparing `unixtime() / 3600` and `unixtime() / 86400` against the last seen value. Hour and day are tracked separately, so reading one flag never consumes the other (a single `lastCheckedTime` would).
* Events are latched, so a rollover is not lost if it is checked late.
* The RTC is read over I2C at most every 250 ms, however often the checks are called.
* `setCurrentTime()` re-baselines the tracking, so changing the clock never produces a false event.
* `wasClockReset()` lets the UI prompt the user to set the clock after a dead or missing backup cell.

#### DataManager
**Implemented:** [`lib/DataManager.h`](../lib/DataManager.h) (header-only; tested with `stand-alone-scripts/DataManagerTest`, including a 1 Hz sensor-plus-SD soak test with zero read or write failures).

Owns the `Adafruit_AS7341` sensor and the SD card (via **SdFat**, not `SD.h` — `SD.h` conflicts with the AS7341). Reads the 8 channels with auto-gain, classifies the light, converts to PAR using `K_SENSOR` from `CAL.CFG`, integrates DLI, and appends CSV records.
```cpp
#include <Adafruit_AS7341.h>
#include <SdFat.h>

class DataManager {
public:
    bool initSensor();
    bool initSD();                       // SHARED_SPI, 4 MHz; also loads K_SENSOR from CAL.CFG
    bool sdAvailable() const;
    bool hasCalibration() const;         // false -> PAR reads 0
    float getKSensor() const;

    bool captureInstantaneousSpectrum(); // blocks ~278 ms (up to 4x with auto-gain); false on read failure
    void processDLIIntegration(float readingDurationSeconds);  // feeds running DLI AND hour stats
    bool commitHourlyRecordToSD(const char* locationName, const DateTime& dt);
    bool rolloverDailyLog(const char* locationName, const DateTime& day);  // `day` = date being CLOSED
    void resetDLICounter();

    const uint16_t* getRawSpectrum() const;
    float getCurrentPAR() const;
    float getCalculatedDLI() const;      // mol m-2 since last reset
    LightType getLightType() const;      // + getLightName()
    float getGainFactor() const;         // gain the stored spectrum was read at
    bool isSaturated() const;            // peak >= 65000 counts
    uint32_t getReadFailures() const;
    uint32_t getWriteFailures() const;
};
```
Design notes:
* **PAR gain:** PAR is normalised by the gain each read was *actually taken at*. The standalone sketches use the gain after the auto-gain step, which is wrong on the rare cycle where the fourth read still needed a change.
* **Failure handling:** a failed capture leaves the previous values in place and `processDLIIntegration()` then skips, so a dropout under-counts rather than re-counting stale light. Hour statistics reset after `commitHourlyRecordToSD()` even if the write fails, so one bad write never pollutes the next hour.
* **Daily rollover:** `rolloverDailyLog()` does not reset counters; the caller then calls `resetDLICounter()`. When triggered by a midnight rollover, pass `now - TimeSpan(1,0,0,0)` as `day`.
* **Output files** (created on first write, with a header row; one file for all locations keeps names 8.3-safe):
  * `HOURLY.CSV` — `hour_end,location,par_avg,par_max,dli_so_far,samples,light_type`
  * `DAILY.CSV` — `date,location,dli_mol_m2,par_peak,samples`
* **Pin setup:** drive `SD_CS` (4) and `TFT_CS` (7) HIGH before any SPI traffic, then `Wire.begin()`, then `initSensor()` / `initSD()`.
* **Low-signal rule:** at very low signal the classifier is unreliable (a covered sensor reads as "Halogen" because red and near-IR leak through the cover). When the gain-normalised peak (counts / gain) is below 1.5 — about 770 counts at 512x — `isDark()` is true, the light type is "Unknown" and PAR is 0. PAR below 0.5 µmol/m²/s is also floored to 0 (12 h at 0.5 is only 0.02 mol/m² of DLI). Thresholds come from covered-sensor readings (peaks of ~170 to ~480 at 512x, depending on how much light leaks through the cover) versus dim indoor light (peak ~1700 at 512x, PAR 1.4).

#### SettingsManager
**Implemented, awaiting hardware test:** [`lib/SettingsManager.h`](../lib/SettingsManager.h) (test sketch: `stand-alone-scripts/SettingsManagerTest`).

Persists user settings on the SD card so the logger can be lent out and moved between places. Location names are written into every `HOURLY.CSV` / `DAILY.CSV` row, so one pair of files covers every location.
```cpp
enum class LocResult { OK, INVALID_NAME, DUPLICATE, FULL, LAST_LOCATION, BAD_INDEX };

class SettingsManager {
public:
    bool begin(SdFat32* fs);                 // nullptr ok; falls back to "DEFAULT_LOC", 60 min
    uint8_t getLocationCount() const;        // up to 10 locations
    const char* getLocation(uint8_t i) const;
    const char* getActiveLocation() const;   uint8_t getActiveIndex() const;
    LocResult addLocation(const char* name);       // 1-15 chars: letters, digits, space _ - .
    LocResult removeLocation(uint8_t i);           // list only; logged data is never touched
    LocResult setActiveLocation(uint8_t i);
    uint16_t getLogIntervalMin() const;      bool setLogIntervalMin(uint16_t m);  // 5/10/15/30/60
    bool saveState(const DliState& s, const char* location);   // running-DLI snapshot
    bool loadState(DliState& s, char* locationOut);
    void clearState();
};
```
Files (8.3 names, `KEY=value` like `CAL.CFG`): `SETTINGS.CFG` (`ACTIVE_LOCATION`, `LOG_INTERVAL_MIN`), `LOCS.CFG` (one name per line), `STATE.CFG` (the `DliState` fields plus the location they belong to). Every write goes to a `.TMP` file first and then replaces the real file; if power is cut between the delete and the rename, the `.TMP` is used at the next load.

Design notes:
* **Switching location is a data boundary.** Before `setActiveLocation()`, the logger writes the current hour and day records under the OLD location and resets the counters (`commitHourlyRecordToSD`, `rolloverDailyLog`, `resetDLICounter`).
* **Surviving resets:** `DataManager::exportState(now)` / `importState()` snapshot the running DLI and hour statistics. The logger saves the snapshot every few minutes and, at boot, restores it only if the saved location matches the active one and the stamps match the current day (and hour). If the snapshot is from an earlier day, the daily row for that day was never written, so the logger should write it from the saved values before discarding them.
* **Clock:** the RTC has no timezone handling. When the logger is lent out to another timezone, set the clock there.
* **`LOG_INTERVAL_MIN` is stored and validated but not yet honored** by `DLI_Logger.ino`, which still writes hourly.

#### UIManager (The Main State Engine)
Orchestrates UX transitions mapped to your specified workflow using a strict finite state configuration.
```cpp
enum class ScreenId {
    MAIN_DISPLAY,
    LIGHT_SIGNATURE,
    SETTINGS_MENU,
    MANAGE_LOCATIONS,
    DATA_ENTRY_KB
};

class UIManager {
private:
    ScreenId currentScreen = ScreenId::MAIN_DISPLAY;
    InputManager* input;
    DataManager* data;
    TimeManager* time;
    
    char currentActiveLocation[16] = "DEFAULT_LOC";
    int menuCursorIndex = 0;
    int kbCursorX = 0, kbCursorY = 0;

    // View Render Sub-methods (Differential drawing execution)
    void renderMainDisplay(bool forceFullRedraw);
    void renderLightSignature(bool forceFullRedraw);
    void renderSettingsMenu(bool forceFullRedraw);
    void renderManageLocations(bool forceFullRedraw);
    void renderDataEntryKB(bool forceFullRedraw);

public:
    UIManager(InputManager* in, DataManager* dt, TimeManager* tm);
    void init();
    void processInputs(); // Evaluates structural conditions from InputManager
    void updateScreen();  // Executes differential drawing routines
    void changeScreen(ScreenId newScreen);
};
```

---

## 4. UI Flow & State Transitions Strategy

The execution of screens matches your architectural menu requirements exactly. Use the following conditions when drafting logic handlers inside `UIManager::processInputs()`:
```
              +-------------------+
              |   MAIN_DISPLAY    | <------------------------+
              +-------------------+                          |
                |               |                            |
[VIEW Pressed]  |               | [SETTINGS Pressed]         |
                v               v                            |
+-------------------+   +-------------------+                |
|  LIGHT_SIGNATURE  |   |   SETTINGS_MENU   |                |
+-------------------+   +-------------------+                |
|                           |                                |
| [BACK/VIEW Pressed]       | [Joystick Ticks]               |
+---------------------------+--> (Select "Manage Locations") |
v                                    |
+-------------------+                |
| MANAGE_LOCATIONS  |                |
+-------------------+                |
|                                    |
| [SELECT Pressed on "Add New"]      | [BACK Pressed]
v                                    |
+-------------------+                |
|   DATA_ENTRY_KB   | ---------------+
+-------------------+
```

### 4.1 State Definitions
1. **`MAIN_DISPLAY`:** Default execution environment. Displays current Location Name, Live PPFD/PAR value, running integrated DLI calculation, Clock, and Battery Status. Pressing **VIEW** moves to `LIGHT_SIGNATURE`. Pressing **SETTINGS** moves to `SETTINGS_MENU`.
2. **`LIGHT_SIGNATURE`:** Renders an 8-channel live dynamic bar graph mapping wavelengths (F1-F8). Auto-scales the height relative to the single highest-amplitude channel. Pressing **BACK** or **VIEW** returns processing straight to `MAIN_DISPLAY`.
3. **`SETTINGS_MENU`:** Linear vertical menu. Use Joystick `UP`/`DOWN` to step through parameters (Set Clock, Logging Interval, Manage Locations). **SELECT** confirms choice; **BACK** cancels back to `MAIN_DISPLAY`.
4. **`MANAGE_LOCATIONS`:** Reads listing profiles. Displays existing tags or an option to "Add New". Clicking "Add New" shifts control straight to the active keyboard text screen.
5. **`DATA_ENTRY_KB`:** Generates a character layout grid. Joystick inputs navigate the grid coordinates. Clicking **SELECT** appends the character. Clicking **BACK** completes string saving or exits.

---

## 5. Step-by-Step Implementation & Test Bench Plan

To guarantee modular reliability, do not write the entire code framework at once. Implement and compile the following phases sequentially. Use the **Test Bench Flags** at the top of the main `.ino` sketch to isolate component boundaries.

### Phase 1: Input Validation Bench (`RUN_TEST_INPUT`)
* **Goal:** Enforce crisp button clicks and verify joystick zones.
* **Implementation:** Setup `InputManager`, wire buttons to `INPUT_PULLUP`, and map standard horizontal/vertical analog channels to `Serial` readouts.
* **Success Metrics:** Buttons trigger a single notification when tapped, and holding them down does not flood the terminal. Moving the joystick left returns exactly `JoystickDir::LEFT` once per activation cycle.

### Phase 2: Chronos & Persistence Check (`RUN_TEST_RTC` & `RUN_TEST_SD`)
* **Goal:** Initialize external I2C RTC and secure functional SPI access to file directory lines.
* **Implementation:** Bring up `TimeManager` and initialize standard file systems. Output automated datetime text formats through the Serial line. Open a test CSV block, dump placeholder strings, and close it cleanly.
* **Success Metrics:** Disconnecting power from the system for 60 seconds does not default the tracking calendar sequence. Inspecting the raw physical SD storage chip on an external PC displays uncorrupted timestamp log chains.

### Phase 3: The Live Spectrometer View (`RUN_SUB_SPECTROMETER`)
* **Goal:** Deploy the differential graphics layout engine and capture multi-spectral channel frames from the AS7341 sensor.
* **Implementation:** Build the self-contained `LightSignatureView` class. Extract raw pixel mapping algorithms. Enforce dynamic auto-scaling algorithms on incoming data packets, updating only modified pixel vertical indices.
* **Success Metrics:** Moving the instrument across indoor spaces shifts individual color columns rapidly (e.g., higher blue ratios under high-temp LEDs vs spikes in red domains under warm accents) without inducing global screen strobing or layout flicker.

### Phase 4: Production Integration
* Disable all local test bench configurations. 
* Incorporate the unified `UIManager` core state machine wrapper.
* Tie mathematical integration loops together to track persistent PAR/DLI data models.

---

## 6. Code Blueprint for Coder Agent

The agent should use this complete, single-file framework layout structure as the operational target or template for building out the individual module classes.

### Complete Orchestrator Layout: `DLI_Logger.ino`
```cpp
/**
 * SPECIFICATION COMPLIANT: DLI_Logger Master Orchestrator
 * Target Architecture: ATmega2560
 * Instructions for Agent: Toggle compilation modes by managing definitions below.
 */

// #define RUN_TEST_INPUT
// #define RUN_TEST_RTC
// #define RUN_TEST_SENSOR
// #define RUN_SUB_SPECTROMETER

#include <Wire.h>
#include <SPI.h>
#include "InputManager.h"
#include "TimeManager.h"
#include "DataManager.h"
#include "UIManager.h"

// System Instance Layer
InputManager inputMgr;
TimeManager  timeMgr;
DataManager  dataMgr;
UIManager* uiMgr;

unsigned long lastSampleMillis = 0;
const unsigned long sampleInterval = 1000; // Sample light physics every 1.0 second

void setup() {
    Serial.begin(9600);
    Wire.begin();

    // ----------------------------------------------------
    // TEST ENVIRONMENT INTERCEPT LOGIC
    // ----------------------------------------------------
    #ifdef RUN_TEST_INPUT
        inputMgr.init();
        Serial.println(F("[BENCH] Input Testing Active."));
        while(1) {
            inputMgr.update();
            if(inputMgr.isButtonPressed(ButtonId::SELECT)) Serial.println(F("SELECT Pressed"));
            if(inputMgr.getJoystickDirection() == JoystickDir::UP) Serial.println(F("Joystick UP"));
            delay(10);
        }
    #endif

    #ifdef RUN_TEST_RTC
        if(!timeMgr.init()) { Serial.println(F("RTC Failed")); while(1); }
        Serial.println(F("[BENCH] Chronos Subsystem Verified."));
        while(1) {
            DateTime now = timeMgr.getCurrentTime();
            Serial.print(now.hour(), DEC); Serial.print(':'); Serial.println(now.minute(), DEC);
            delay(1000);
        }
    #endif

    // ----------------------------------------------------
    // FULL PRODUCTION SYSTEM INITIALIZATION
    // ----------------------------------------------------
    inputMgr.init();
    
    if(!timeMgr.init()) { Serial.println(F("Critical Failure: RTC Init.")); }
    if(!dataMgr.initSensor()) { Serial.println(F("Critical Failure: AS7341 Init.")); }
    if(!dataMgr.initSD()) { Serial.println(F("Warning: Storage Offline.")); }

    uiMgr = new UIManager(&inputMgr, &dataMgr, &timeMgr);
    uiMgr->init();
    
    Serial.println(F("System Engine Bootstrapped Successfully."));
}

void loop() {
    // 1. Keep raw physical input filters refreshed continuously
    inputMgr.update();

    // 2. Map evaluation ticks straight into UI Engine state controllers
    uiMgr->processInputs();

    // 3. Process algorithmic data accumulation intervals non-blockingly
    unsigned long currentMillis = millis();
    if (currentMillis - lastSampleMillis >= sampleInterval) {
        float timeDeltaSeconds = (currentMillis - lastSampleMillis) / 1000.0f;
        lastSampleMillis = currentMillis;

        // Collect register data, compute instant PAR; integrate only on success
        if (dataMgr.captureInstantaneousSpectrum()) {
            // Accumulate photon counts mathematically into total DLI values
            dataMgr.processDLIIntegration(timeDeltaSeconds);
        }

        // Track calendar rollover conditions (at midnight both fire; hour first)
        if (timeMgr.hasHourRolledOver()) {
            dataMgr.commitHourlyRecordToSD("LIVING_RM", timeMgr.getCurrentTime());
        }
        if (timeMgr.hasDayRolledOver()) {
            // The day being closed is yesterday
            dataMgr.rolloverDailyLog("LIVING_RM", timeMgr.getCurrentTime() - TimeSpan(1, 0, 0, 0));
            dataMgr.resetDLICounter();
        }
    }

    // 4. Force differential UI rendering updates
    uiMgr->updateScreen();
}
```

---

## 7. Memory Optimization Rules for Coder Agent
To prevent stability issues on the ATmega2560 framework, ensure the following engineering constraints are met during class implementation:
1. **Enforce `F()` Macro Utilization:** Wrap all literal print strings across the UI space inside the Flash storage directive macro. Example: `Serial.println(F("Booting..."));` or `tft.print(F("PAR:"));`. This preserves dynamic RAM.
2. **Strict Avoidance of `std::string`:** Do not use C++ standard string class mechanics, as they cause dynamic heap fragmentation and memory leaks on small microcontrollers. Use traditional, fixed-size null-terminated character arrays (`char text[16]`).
3. **Prohibit Heap Allocations in Loops:** Do not use the `new` or `malloc` operators within periodic tasks or the main `loop()` function. Instantiate all manager instances either globally or statically during initial system startup.

---

## 8. Spectrometer Subsystem

**Sketch:** [`stand-alone-scripts/Spectrometer/Spectrometer.ino`](../stand-alone-scripts/Spectrometer/Spectrometer.ino)

This is a standalone validation and calibration tool — it is not part of the main DLI logger sketch. Its job is to let you point the sensor at any light source, immediately see its spectral fingerprint as a colour bar chart on the TFT, and read the light type classification on-screen. It also streams raw channel counts to the Serial Monitor in CSV format so readings can be logged for PAR calibration work.

### How a reading is taken

Every 500ms the sketch asks the AS7341 sensor to take a fresh measurement. The sensor simultaneously samples eight narrow wavelength bands (415, 445, 480, 515, 555, 590, 630, and 680nm) and returns a 16-bit count for each. Higher counts mean more photons at that wavelength hit the sensor during the integration window.

An **integration time** of approximately 278ms is used (ATIME=99, ASTEP=999). The sensor blocks until the integration is complete, so no software delay is needed after triggering a read.

### Auto-gain

The AS7341 has an internal amplifier whose gain can be set from 0.5× up to 512×. The sketch adjusts this automatically so the readings stay in a useful range regardless of how bright or dim the source is:

- If the brightest channel reaches 60,000 counts (near the 16-bit ceiling), gain steps down one level and the sensor re-reads immediately.
- If the brightest channel is below 1,000 counts (too dim to see fine detail), gain steps up one level and re-reads.
- This repeats up to four times per cycle, then accepts whatever result it has.

The current gain multiplier (e.g. "G:256x") is shown in the top-right of the display and updates whenever it changes. Bright outdoor light typically settles around 64× or lower; dim indoor sources often reach 512×.

### Bar chart display

The eight channels are drawn as coloured bars across the screen — violet on the left through red on the right, matching the visible spectrum. Each bar's height is scaled relative to the tallest channel in the current reading, so the chart always fills the available space. Only the pixels that actually changed height are redrawn each cycle, which keeps the display flicker-free.

### Light type classification

After each read the sketch examines the shape of the spectrum — the relative proportions of blue (415–480nm), green (515–555nm), yellow (590nm), and red (630–680nm) energy — and shows a plain-English label at the bottom of the screen. The logic works as follows:

1. **Sunlight / Window Light** — Natural daylight has a broad, relatively flat spectrum. If the ratio of the dimmest channel to the brightest is above 0.15 AND there is meaningful energy in both the blue and red ends, the source is classified as daylight. Because window glass passes all visible wavelengths including 415nm violet, indoor window light looks spectrally identical to outdoor sky. The gain level is what separates them: if the sensor needed high gain (≥128×) to see the light it is dim, meaning you are indoors by a window ("Window Light"); if it needed low gain the light is bright direct sunlight.

2. **Cool LED** — If more than 35% of the total counts come from the blue channels (415–480nm), the source is a cool-white LED.

3. **Halogen vs. Warm LED** — If more than 30% of counts come from the red channels (630–680nm), the source is either warm-white or halogen. These two are separated by looking at the ratio of the 680nm channel to the 630nm channel. A halogen bulb is a blackbody radiator with a smooth continuous spectrum, so 680nm energy is roughly equal to or greater than 630nm energy (ratio > 0.75). A warm LED has a phosphor peak near 630nm that drops off sharply toward the infrared, so its 680nm reading is noticeably lower than 630nm (ratio < 0.75).

4. **HPS / HID** — If the green band (515–555nm) dominates at above 40%, the source is a high-pressure sodium or HID lamp.

5. **Fluorescent** — Warm-white fluorescent tubes have a strong yellow phosphor emission at 590nm in addition to broad green energy. If the combined green-plus-yellow fraction (515nm + 555nm + 590nm) exceeds 42% with a moderate blue component, the source is classified as fluorescent.

Note: broad-spectrum grow lights are spectrally indistinguishable from warm white LED at 8-channel resolution and will be classified as Warm LED. The DLI logger will handle this with a user-configurable PAR conversion factor per location.

### Serial CSV output

Each cycle the sketch prints one line to Serial (115200 baud) in the format:
```
F1(415),F2(445),F3(480),F4(515),F5(555),F6(590),F7(630),F8(680),gain_x
```
This data was used to build the lux-to-PAR calibration dataset (10 locations, documented in `memory/project_calibration.md`) which provides the per-light-type conversion factors the DLI logger will use.

---

## 9. Implementation Status

Hardware is complete (Fritzing schematic: `docs/Complete Circuit.fzz`). Each component below lives in `lib/` as a header-only library with a stand-alone test sketch in `stand-alone-scripts/`. A sketch needs its own copy of each header it uses (copy from `lib/`); edit `lib/` and re-copy to avoid the copies drifting.

| Component | Library | Test / tool sketch | Status |
| :--- | :--- | :--- | :--- |
| `InputManager` | `lib/InputManager.h` | `InputTest` | Done, hardware-tested |
| `TimeManager` | `lib/TimeManager.h` | `TimeManagerTest` | Done, hardware-tested |
| `DataManager` | `lib/DataManager.h` | `DataManagerTest` | Done, hardware-tested (incl. 1 Hz sensor + SD soak) |
| Spectrometer / light classifier | inline in sketch | `Spectrometer` | Done; the bar-chart code is to be extracted into a `LightSignatureView` |
| PAR calibration (`K_SENSOR`, `CAL.CFG`) | inline in sketch | `Calibration` | Done |
| TFT, SD and RTC bring-up | — | `TFT_Test`, `SDTest`, `clock_test` | Done |
| `SettingsManager` (locations, active location, logging interval, running-DLI snapshot) | `lib/SettingsManager.h` | `SettingsManagerTest` | Written, awaiting hardware test. Not yet wired into `DLI_Logger.ino` |
| `UIManager` and screens | — | — | **Not started** — `MAIN_DISPLAY`, `LIGHT_SIGNATURE`, `SETTINGS_MENU`, `MANAGE_LOCATIONS`, `DATA_ENTRY_KB` |
| `DLI_Logger.ino` orchestrator | uses the three managers | `DLI_Logger/` | **Headless version written, awaiting overnight hardware test.** Samples at 1 Hz, writes hourly / daily CSVs, status over Serial, recovers from missing hardware. No display yet. |

Planned order: run the headless logger overnight and verify the hourly rows and the midnight daily row; then build `UIManager` (`MAIN_DISPLAY`, then `LIGHT_SIGNATURE`), add settings persistence, and add the menu screens one at a time.

Integration notes for the orchestrator:
* `captureInstantaneousSpectrum()` blocks for ~278 ms (up to ~1.1 s with auto-gain stepping). Poll `InputManager` between reads; it latches button presses, but joystick auto-repeat will stutter during a read.
* SD and TFT share the SPI bus: drive both chip-selects HIGH at the very top of `setup()`, before any SPI traffic.
