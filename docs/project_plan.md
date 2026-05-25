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

### 2.2 Shared SPI Bus (Pins 50, 51, 52)
* **MISO:** Pin 50 (Master In Slave Out - Connected to SD Card Slot out)
* **MOSI (DIN):** Pin 51 (Master Out Slave In - Connected to TFT DIN and SD Card MOSI)
* **SCK (CLK):** Pin 52 (Serial Clock - Connected to TFT CLK and SD Card CLK)

### 2.3 Individual Control/Select Pins

| Component Module | Component Pin | Mega 2560 Pin | Config / Mode | Description |
| :--- | :--- | :--- | :--- | :--- |
| **1.8" ST7735 TFT** | 1-RST | Pin 8 | Output | Hardware Reset Line |
| **1.8" ST7735 TFT** | 2-CS | Pin 10 | Output | TFT Chip Select Line |
| **1.8" ST7735 TFT** | 3-D/C | Pin 9 | Output | Data / Command Toggle Line |
| **1.8" ST7735 TFT** | 7-BL | Pin 7 | Output (PWM) | Backlight Brightness Control |
| **MicroSD Card Slot**| SD_CS | Pin 4 | Output | SD Card Chip Select Line |
| **Tactile Button A** | Switch Terminal 1 | Pin 22 | INPUT_PULLUP | Reads LOW when pressed |
| **Tactile Button B** | Switch Terminal 1 | Pin 23 | INPUT_PULLUP | Reads LOW when pressed |
| **Tactile Button VIEW**| Switch Terminal 1 | Pin 24 | INPUT_PULLUP | Reads LOW when pressed |
| **Tactile Button SET** | Switch Terminal 1 | Pin 25 | INPUT_PULLUP | Reads LOW when pressed |
| **Thumb Joystick X** | VRx (Horizontal) | Pin A0 | Analog Input | Left/Right Position |
| **Thumb Joystick Y** | VRy (Vertical)   | Pin A1 | Analog Input | Up/Down Position |

*Note: All secondary terminals for the tactile buttons, as well as the GND pins of the joystick, sensor, RTC, TFT, and SD reader, must connect directly to the common ground rail.*

### 2.4 TFT Display Library & Level Shifting

**Library: TFT_eSPI by Bodmer.** Do NOT use Adafruit_GFX or Adafruit_ST7735 — those libraries could not produce correct pixel output on this hardware. TFT_eSPI is configured entirely through its `User_Setup.h` file (in the Arduino libraries folder); no pin arguments are passed to the constructor.

**Level shifting (5V Mega → 3.3V display).** The ST7735 TFT runs on 3.3V logic. Five signal lines require voltage dividers: CS (pin 10), RST (pin 8), DC (pin 9), MOSI (pin 51), and CLK (pin 52). Each divider uses a **330Ω resistor on top** (between the Mega output and the TFT pin) and a **680Ω resistor to GND** on the bottom. This produces approximately 3.37V at the TFT input and a source impedance of roughly 220Ω — low enough to avoid signal rounding at 8 MHz SPI.

> **Do not use high-value resistors** (e.g. 3kΩ/5.6kΩ). The resulting ~2kΩ Thevenin impedance forms an RC low-pass filter with wire capacitance that rounds SPI edges faster than the bit period, corrupting all pixel writes even though the slower init sequence appears to succeed (symptom: fillScreen has no effect).

---

## 3. Object-Oriented Software Design Spec

To maximize architectural maintainability, all domain spaces are modeled inside discrete classes. 

### 3.1 Class Diagrams & Interfaces

#### InputManager
Enforces button debouncing using non-blocking software timers and converts analog joystick sweeps into precise discrete movements using strict window thresholds.
```cpp
enum class ButtonId { A, B, VIEW, SETTINGS };
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
Wraps the Adafruit `RTClib` and handles non-blocking temporal comparisons to emit events when a standard hour or calendar day has crossed structural thresholds.
```cpp
#include <RTClib.h>

class TimeManager {
private:
    RTC_DS3231 rtc;
    DateTime lastCheckedTime;

public:
    bool init();
    DateTime getCurrentTime();
    void setCurrentTime(uint16_t year, uint8_t month, uint8_t day, uint8_t hour, uint8_t min, uint8_t sec);
    bool hasHourRolledOver();
    bool hasDayRolledOver();
};
```

#### DataManager
Interfaces with the `Adafruit_AS7341` library, applies calibrated algorithms to approximate PAR, integrates historical arrays, and logs entries to the SD Card reader.
```cpp
#include <Adafruit_AS7341.h>
#include <SD.h>

struct SpectralSnapshot {
    uint16_t channels[8]; // F1-F8 values
    float par;
    uint32_t timestamp;
};

class DataManager {
private:
    Adafruit_AS7341 sensor;
    const int chipSelectSD = 4;
    
    float currentPAR = 0.0f;
    float cumulativeDLI = 0.0f;
    unsigned long readingCount = 0;
    
    uint16_t currentRawSpectrum[8];
    float hourlyPARHistory[24];
    uint8_t historyIndex = 0;

public:
    bool initSensor();
    bool initSD();
    
    void captureInstantaneousSpectrum();
    void processDLIIntegration(float readingDurationSeconds);
    void rolloverDailyLog(const char* locationName, DateTime date);
    bool commitHourlyRecordToSD(const char* locationName, DateTime dt);
    
    const uint16_t* getRawSpectrum() const { return currentRawSpectrum; }
    float getCurrentPAR() const { return currentPAR; }
    float getCalculatedDLI() const { return cumulativeDLI; }
    void resetDLICounter();
};
```

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
| [B/VIEW Pressed]          | [Joystick Ticks]               |
+---------------------------+--> (Select "Manage Locations") |
v                                    |
+-------------------+                |
| MANAGE_LOCATIONS  |                |
+-------------------+                |
|                                    |
| [A Pressed on "Add New"]           | [B Pressed]
v                                    |
+-------------------+                |
|   DATA_ENTRY_KB   | ---------------+
+-------------------+
```

### 4.1 State Definitions
1. **`MAIN_DISPLAY`:** Default execution environment. Displays current Location Name, Live PPFD/PAR value, running integrated DLI calculation, Clock, and Battery Status. Pressing **VIEW** moves to `LIGHT_SIGNATURE`. Pressing **SETTINGS** moves to `SETTINGS_MENU`.
2. **`LIGHT_SIGNATURE`:** Renders an 8-channel live dynamic bar graph mapping wavelengths (F1-F8). Auto-scales the height relative to the single highest-amplitude channel. Pressing **B** or **VIEW** returns processing straight to `MAIN_DISPLAY`.
3. **`SETTINGS_MENU`:** Linear vertical menu. Use Joystick `UP`/`DOWN` to step through parameters (Set Clock, Logging Interval, Manage Locations). **Button A** confirms choice; **Button B** cancels back to `MAIN_DISPLAY`.
4. **`MANAGE_LOCATIONS`:** Reads listing profiles. Displays existing tags or an option to "Add New". Clicking "Add New" shifts control straight to the active keyboard text screen.
5. **`DATA_ENTRY_KB`:** Generates a character layout grid. Joystick inputs navigate the grid coordinates. Clicking **Button A** appends the character. Clicking **Button B** completes string saving or exits.

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
            if(inputMgr.isButtonPressed(ButtonId::A)) Serial.println(F("Button A Clicked"));
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

        // Collect register data, compute instant PAR
        dataMgr.captureInstantaneousSpectrum();
        
        // Accumulate photon counts mathematically into total DLI values
        dataMgr.processDLIIntegration(timeDeltaSeconds);

        // Track calendar rollover conditions
        if (timeMgr.hasHourRolledOver()) {
            dataMgr.commitHourlyRecordToSD("LIVING_RM", timeMgr.getCurrentTime());
        }
        if (timeMgr.hasDayRolledOver()) {
            dataMgr.rolloverDailyLog("LIVING_RM", timeMgr.getCurrentTime());
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

**Sketch:** [`Spectrometer/Spectrometer.ino`](../Spectrometer/Spectrometer.ino)

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