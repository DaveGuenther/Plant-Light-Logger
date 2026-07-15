/**
 * Calibration.ino
 * Derives the AS7341→PAR sensor constant K_sensor by comparing bandwidth-
 * weighted channel counts against a handheld lux meter reading.
 *
 * Workflow:
 *   1. Point the sensor at the light source you want to calibrate under.
 *   2. Watch the spectral bars stabilise and auto-classify the light type.
 *   3. Read your LX1330B lux meter, then press SELECT.
 *   4. Use the joystick to dial in the lux value:
 *        UP/DOWN = change highlighted digit    LEFT/RIGHT = move cursor
 *        SELECT = confirm                      BACK = cancel
 *   5. K_sensor and estimated PPFD are shown and saved to CAL.CFG on the SD card.
 *
 * Using K_sensor in the main logger (DataManager):
 *   const float CH_BW[8] = {26,30,36,39,39,40,50,52};
 *   float normSum = 0;
 *   for (uint8_t i=0; i<8; i++) normSum += ch[i] / gainFactor * CH_BW[i];
 *   float PAR_umol_m2_s = K_SENSOR * normSum;
 *
 * Wiring (identical to Spectrometer.ino, plus SD card + InputManager):
 *   TFT RST->5  CS->7  DC->6  BL->44(68Ω)  MOSI->51  CLK->52
 *   AS7341 SDA->20  SCL->21
 *   SD CS->4  SD MOSI->51  SD CLK->52  SD MISO->50
 *   Buttons A/B/VIEW/SET -> Pins 22/23/24/25 (other terminal to GND)
 *   Joystick X->A1  Y->A0
 *
 * Libraries (all via Library Manager):
 *   Adafruit GFX, Adafruit ST7735 and ST7789, Adafruit AS7341, RTClib, SD
 *
 * NOTE: copy InputManager.h from lib/ into this sketch folder before compiling.
 */

#include <Wire.h>
#include <SPI.h>
#include <SdFat.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Adafruit_AS7341.h>
#include <RTClib.h>
#include "InputManager.h"

// ---------------------------------------------------------------------------
// Pin definitions
// ---------------------------------------------------------------------------
#define TFT_CS  7
#define TFT_DC  6
#define TFT_RST 5
#define TFT_BL  44
#define SD_CS   4

// ---------------------------------------------------------------------------
// Layout constants (160×128 landscape)
//
//  y=0   ┌──────────────────────────────────────┐
//        │ PAR CALIBRATION            G:256x    │  16px header
//  y=16  ├──────────────────────────────────────┤
//        │         spectral bar chart           │  55px
//  y=71  ├──────────────────────────────────────┤
//        │  415  445  480  515  555  590  630  680│  12px wavelength labels
//  y=83  ├──────────────────────────────────────┤
//        │  LIGHT: Warm LED                     │  10px light type
//  y=93  ├──────────────────────────────────────┤
//        │  [status / lux entry / result]       │  35px
//  y=128 └──────────────────────────────────────┘
// ---------------------------------------------------------------------------
#define SCREEN_W   160
#define SCREEN_H   128

#define CHART_TOP  17
#define CHART_H    54
#define CHART_BOT  (CHART_TOP + CHART_H)   // y=71

#define LABEL_Y    73
#define TYPE_Y     84
#define STATUS_Y   94      // top of status / entry area

#define GAIN_X     114     // x-start of gain label in header

// 8 bars across 160px: 20px slot, 14px bar, 3px left gap
#define BAR_SLOT   20
#define BAR_W      14
#define BAR_X(i)   ((i)*BAR_SLOT + 3)

// ---------------------------------------------------------------------------
// Sensor calibration data
// ---------------------------------------------------------------------------

// AS7341 channel bandwidths in nm (FWHM, from datasheet).
// Used to weight each channel's contribution to total photon flux.
static const float CH_BW[8] = {26.f, 30.f, 36.f, 39.f, 39.f, 40.f, 50.f, 52.f};

// Lux-to-PAR conversion factors (µmol m⁻² s⁻¹ per lux) per light type.
// Derived from 10-location LX1330B + AS7341 survey (see memory/project_calibration.md).
static const float LUX_TO_PAR[8] = {
    0.019f,   // LT_SUNLIGHT
    0.0175f,  // LT_FILTERED_DAYLIGHT
    0.0155f,  // LT_COOL_LED
    0.013f,   // LT_WARM_LED
    0.015f,   // LT_HALOGEN
    0.015f,   // LT_FLUORESCENT
    0.014f,   // LT_HPS_HID
    0.015f,   // LT_UNKNOWN
};

// Gain multiplier values; index matches as7341_gain_t enum (0=0.5×, 1=1×, …, 10=512×)
static const float GAIN_VALUES[11] = {
    0.5f, 1.f, 2.f, 4.f, 8.f, 16.f, 32.f, 64.f, 128.f, 256.f, 512.f
};

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

enum LightType : uint8_t {
    LT_SUNLIGHT = 0, LT_FILTERED_DAYLIGHT, LT_COOL_LED, LT_WARM_LED,
    LT_HALOGEN, LT_FLUORESCENT, LT_HPS_HID, LT_UNKNOWN
};

static const char* LIGHT_NAMES[8] = {
    "Sunlight", "Window Light", "Cool LED", "Warm LED",
    "Halogen",  "Fluorescent",  "HPS/HID",  "Unknown"
};

enum class CalState : uint8_t { MEASURING, LUX_ENTRY, COMPUTING, RESULT };

// ---------------------------------------------------------------------------
// Colour palette
// ---------------------------------------------------------------------------
static const uint16_t BAR_COLORS[8] = {
    0x780F,  // 415nm – purple
    0x001F,  // 445nm – blue
    0x055F,  // 480nm – blue-cyan
    0x07FF,  // 515nm – cyan
    0x07E0,  // 555nm – green
    0xFFE0,  // 590nm – yellow
    0xFC60,  // 630nm – orange
    0xF800,  // 680nm – red
};
static const char* WL_LABELS[8] = {"415","445","480","515","555","590","630","680"};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
Adafruit_ST7735  tft(TFT_CS, TFT_DC, TFT_RST);
Adafruit_AS7341  sensor;
RTC_DS3231       rtc;
InputManager     inputMgr;
SdFat32          sd;

static CalState       calState       = CalState::MEASURING;
static as7341_gain_t  currentGain    = AS7341_GAIN_256X;
static as7341_gain_t  prevDispGain   = (as7341_gain_t)255;  // sentinel
static uint16_t       ch[8]          = {};
static uint8_t        prevBarH[8]    = {};
static uint8_t        prevLightType  = 255;                 // sentinel
static LightType      detectedLight  = LT_UNKNOWN;
static bool           sdAvailable    = false;
static bool           rtcAvailable   = false;

// LUX digit entry (5 digits, index 0 = most-significant)
static uint8_t  luxDigits[5] = {0, 0, 0, 0, 0};
static uint8_t  luxCursor    = 4;   // start at units place

// Computed results
static float    K_sensor   = 0.f;
static float    estPPFD    = 0.f;
static float    normSum    = 0.f;
static bool     sdSaveOk   = false;

// ---------------------------------------------------------------------------
// Light classification (mirrors Spectrometer.ino)
// ---------------------------------------------------------------------------
LightType classifyLight(const uint16_t* c) {
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

    if (balance > 0.15f && blue > 0.15f && red > 0.12f)
        return (currentGain >= AS7341_GAIN_128X) ? LT_FILTERED_DAYLIGHT : LT_SUNLIGHT;
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

// ---------------------------------------------------------------------------
// Auto-gain
// ---------------------------------------------------------------------------
bool autoAdjustGain(const uint16_t* c) {
    uint16_t peak = 0;
    for (uint8_t i = 0; i < 8; i++) if (c[i] > peak) peak = c[i];
    if (peak >= 60000 && currentGain > AS7341_GAIN_0_5X) {
        currentGain = (as7341_gain_t)((int)currentGain - 1);
        sensor.setGain(currentGain);
        return true;
    }
    if (peak < 1000 && currentGain < AS7341_GAIN_512X) {
        currentGain = (as7341_gain_t)((int)currentGain + 1);
        sensor.setGain(currentGain);
        return true;
    }
    return false;
}

static const char* gainLabel(as7341_gain_t g) {
    switch (g) {
        case AS7341_GAIN_0_5X:  return "G:0.5x";
        case AS7341_GAIN_1X:    return "G:1x";
        case AS7341_GAIN_2X:    return "G:2x";
        case AS7341_GAIN_4X:    return "G:4x";
        case AS7341_GAIN_8X:    return "G:8x";
        case AS7341_GAIN_16X:   return "G:16x";
        case AS7341_GAIN_32X:   return "G:32x";
        case AS7341_GAIN_64X:   return "G:64x";
        case AS7341_GAIN_128X:  return "G:128x";
        case AS7341_GAIN_256X:  return "G:256x";
        case AS7341_GAIN_512X:  return "G:512x";
        default:                return "G:???";
    }
}

// ---------------------------------------------------------------------------
// Display: static chrome (drawn once)
// ---------------------------------------------------------------------------
void drawChrome() {
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(2, 2);
    tft.print(F("PAR CALIBRATION"));
    tft.drawFastHLine(0, CHART_TOP - 1, SCREEN_W, 0x2945);
    tft.drawFastHLine(0, CHART_BOT + 1, SCREEN_W, 0x2945);
    tft.setTextColor(0x7BEF);
    for (uint8_t i = 0; i < 8; i++) {
        tft.setCursor(BAR_X(i) - 2, LABEL_Y);
        tft.print(WL_LABELS[i]);
    }
    tft.drawFastHLine(0, TYPE_Y  - 2, SCREEN_W, 0x2945);
    tft.drawFastHLine(0, STATUS_Y - 2, SCREEN_W, 0x2945);
}

// ---------------------------------------------------------------------------
// Display: gain label (top-right, updates only when gain changes)
// ---------------------------------------------------------------------------
void updateGainDisplay() {
    if (currentGain == prevDispGain) return;
    prevDispGain = currentGain;
    tft.fillRect(GAIN_X, 0, SCREEN_W - GAIN_X, 15, ST77XX_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(0x7BEF);
    tft.setCursor(GAIN_X, 2);
    tft.print(gainLabel(currentGain));
}

// ---------------------------------------------------------------------------
// Display: spectral bar chart (differential — only changed pixels redrawn)
// ---------------------------------------------------------------------------
void updateBars(const uint16_t* c) {
    uint16_t peak = 1;
    for (uint8_t i = 0; i < 8; i++) if (c[i] > peak) peak = c[i];
    for (uint8_t i = 0; i < 8; i++) {
        uint8_t newH = (uint8_t)((uint32_t)c[i] * CHART_H / peak);
        if (newH == prevBarH[i]) continue;
        uint8_t x      = BAR_X(i);
        uint8_t newTop = CHART_BOT - newH;
        uint8_t oldTop = CHART_BOT - prevBarH[i];
        if (newH > prevBarH[i])
            tft.fillRect(x, newTop, BAR_W, newH - prevBarH[i], BAR_COLORS[i]);
        else
            tft.fillRect(x, oldTop, BAR_W, prevBarH[i] - newH, ST77XX_BLACK);
        prevBarH[i] = newH;
    }
}

// ---------------------------------------------------------------------------
// Display: light type row (updates only when classification changes)
// ---------------------------------------------------------------------------
void updateLightTypeRow(LightType lt) {
    if ((uint8_t)lt == prevLightType) return;
    prevLightType = (uint8_t)lt;
    tft.fillRect(0, TYPE_Y, SCREEN_W, STATUS_Y - 2 - TYPE_Y, ST77XX_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_YELLOW);
    tft.setCursor(2, TYPE_Y);
    tft.print(F("LIGHT: "));
    tft.setTextColor(ST77XX_WHITE);
    tft.print(LIGHT_NAMES[lt]);
}

// ---------------------------------------------------------------------------
// Display: status area — cleared and redrawn on each state transition
// ---------------------------------------------------------------------------
void clearStatusArea() {
    tft.fillRect(0, STATUS_Y, SCREEN_W, SCREEN_H - STATUS_Y, ST77XX_BLACK);
}

void drawStatus_Measuring() {
    clearStatusArea();
    tft.setTextSize(1);
    tft.setTextColor(0x7BEF);
    tft.setCursor(2, STATUS_Y + 3);
    tft.print(F("Hold still. Read lux meter."));
    tft.setTextColor(ST77XX_GREEN);
    tft.setCursor(2, STATUS_Y + 15);
    tft.print(F("[SELECT] Enter lux value"));
}

// Draws the static parts of the LUX entry screen (label + instruction)
void drawStatus_LuxEntry_Static() {
    clearStatusArea();
    tft.setTextSize(1);
    tft.setTextColor(0x7BEF);
    tft.setCursor(2, STATUS_Y + 3);
    tft.print(F("LUX:"));
    tft.setTextColor(0x4208);
    tft.setCursor(2, STATUS_Y + 27);
    tft.print(F("J:+/-  SEL:ok  BACK:cancel"));
}

// Redraws just the 5 digit boxes (called each loop iteration in LUX_ENTRY state)
void drawLuxDigits() {
    // 5 boxes, each 14px wide, 18px tall, 2px gap → total 14*5 + 2*4 = 78px
    // Centred in 160px: x start = (160-78)/2 = 41
    // But we have "LUX:" at x=2..27, so shift right slightly:
    // Place digits starting at x=45 → 45 + 78 = 123, leaving 37px on right. Fine.
    const uint8_t BOX_W   = 14;
    const uint8_t BOX_H   = 18;
    const uint8_t BOX_GAP = 3;
    const uint8_t startX  = 45;
    const uint8_t startY  = STATUS_Y + 5;

    for (uint8_t i = 0; i < 5; i++) {
        uint8_t bx  = startX + i * (BOX_W + BOX_GAP);
        bool    sel = (i == luxCursor);

        uint16_t bgColor   = sel ? ST77XX_YELLOW : ST77XX_BLACK;
        uint16_t fgColor   = sel ? ST77XX_BLACK  : ST77XX_WHITE;
        uint16_t rimColor  = 0x4208;   // dark grey outline

        tft.fillRect(bx, startY, BOX_W, BOX_H, bgColor);
        if (!sel) tft.drawRect(bx, startY, BOX_W, BOX_H, rimColor);

        tft.setTextColor(fgColor);
        tft.setTextSize(1);
        // Centre the digit character (6×8) inside the 14×18 box
        tft.setCursor(bx + 4, startY + 5);
        tft.print(luxDigits[i]);
    }
}

void drawStatus_Result() {
    clearStatusArea();
    tft.setTextSize(1);

    if (sdSaveOk) {
        tft.setTextColor(ST77XX_GREEN);
    } else {
        tft.setTextColor(ST77XX_YELLOW);
    }
    tft.setCursor(2, STATUS_Y + 2);
    tft.print(F("K="));
    tft.print(K_sensor, 6);

    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(2, STATUS_Y + 12);
    tft.print(F("PPFD~"));
    tft.print(estPPFD, 1);
    tft.print(F(" umol/m2/s"));

    tft.setCursor(2, STATUS_Y + 22);
    if (sdSaveOk) {
        tft.setTextColor(0x7BEF);
        tft.print(F("Saved: CAL.CFG  [SEL]=redo"));
    } else {
        tft.setTextColor(ST77XX_RED);
        tft.print(F("SD save failed  [SEL]=redo"));
    }
}

// ---------------------------------------------------------------------------
// Sensor read with auto-gain (up to 4 attempts)
// ---------------------------------------------------------------------------
bool readSensor() {
    for (uint8_t attempt = 0; attempt < 4; attempt++) {
        inputMgr.update();   // latch inputs between blocking reads (~278ms each)
        if (!sensor.readAllChannels()) return false;
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
    return true;
}

// ---------------------------------------------------------------------------
// Compute K_sensor and save to SD card
// ---------------------------------------------------------------------------
void computeAndSave() {
    // Bandwidth-weighted, gain-normalised sensor sum
    float gainFactor = GAIN_VALUES[(int)currentGain];
    normSum = 0.f;
    for (uint8_t i = 0; i < 8; i++) {
        normSum += (float)ch[i] / gainFactor * CH_BW[i];
    }

    // PPFD estimate from lux and light-type conversion factor
    float luxVal = luxDigits[0] * 10000.f
                 + luxDigits[1] *  1000.f
                 + luxDigits[2] *   100.f
                 + luxDigits[3] *    10.f
                 + luxDigits[4];
    estPPFD  = luxVal * LUX_TO_PAR[(int)detectedLight];
    K_sensor = (normSum > 0.f) ? estPPFD / normSum : 0.f;

    Serial.print(F("K_sensor = "));   Serial.println(K_sensor, 8);
    Serial.print(F("normSum  = "));   Serial.println(normSum, 2);
    Serial.print(F("estPPFD  = "));   Serial.println(estPPFD, 3);
    Serial.print(F("LUX      = "));   Serial.println(luxVal, 0);
    Serial.print(F("LightType= "));   Serial.println(LIGHT_NAMES[(int)detectedLight]);

    if (!sdAvailable) {
        sdSaveOk = false;
        return;
    }

    sd.remove("CAL.CFG");
    File32 f = sd.open("CAL.CFG", O_WRITE | O_CREAT | O_TRUNC);
    if (!f) {
        sdSaveOk = false;
        return;
    }

    f.println(F("# AS7341 PAR Calibration"));
    f.print(F("K_SENSOR="));       f.println(K_sensor, 8);
    f.print(F("LIGHT_TYPE_ID="));  f.println((int)detectedLight);
    f.print(F("LIGHT_TYPE="));     f.println(LIGHT_NAMES[(int)detectedLight]);
    f.print(F("LUX_REF="));        f.println(luxVal, 0);
    f.print(F("PPFD_EST="));       f.println(estPPFD, 3);
    f.print(F("NORM_SUM="));       f.println(normSum, 2);
    f.print(F("GAIN="));           f.println(gainFactor, 1);

    if (rtcAvailable) {
        DateTime now = rtc.now();
        f.print(F("DATE="));
        f.print(now.year());  f.print('-');
        if (now.month()  < 10) f.print('0'); f.print(now.month());  f.print('-');
        if (now.day()    < 10) f.print('0'); f.print(now.day());    f.print(' ');
        if (now.hour()   < 10) f.print('0'); f.print(now.hour());   f.print(':');
        if (now.minute() < 10) f.print('0'); f.print(now.minute()); f.print(':');
        if (now.second() < 10) f.print('0'); f.println(now.second());
    }

    f.close();
    sdSaveOk = true;
}

// ---------------------------------------------------------------------------
// Transition to a new state
// ---------------------------------------------------------------------------
void enterState(CalState next) {
    calState = next;
    switch (next) {
        case CalState::MEASURING:
            drawStatus_Measuring();
            break;
        case CalState::LUX_ENTRY:
            luxDigits[0] = luxDigits[1] = luxDigits[2] = luxDigits[3] = luxDigits[4] = 0;
            luxCursor = 4;
            drawStatus_LuxEntry_Static();
            drawLuxDigits();
            break;
        case CalState::COMPUTING:
            // instant — no display needed, handled in loop()
            break;
        case CalState::RESULT:
            drawStatus_Result();
            break;
    }
}

// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);

    // Deassert SD CS before TFT SPI starts — prevents TFT init traffic
    // from being seen by the SD card as valid SPI commands.
    pinMode(SD_CS, OUTPUT);
    digitalWrite(SD_CS, HIGH);

    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);

    tft.initR(INITR_BLACKTAB);
    tft.setRotation(1);
    drawChrome();

    // --- RTC ---
    Wire.begin();
    rtcAvailable = rtc.begin();
    if (rtcAvailable && rtc.lostPower()) {
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }

    // --- AS7341 ---
    if (!sensor.begin()) {
        tft.fillScreen(ST77XX_BLACK);
        tft.setTextColor(ST77XX_RED);
        tft.setTextSize(1);
        tft.setCursor(4, 50);
        tft.print(F("AS7341 NOT FOUND"));
        tft.setCursor(4, 62);
        tft.print(F("Check SDA(20)/SCL(21)"));
        Serial.println(F("AS7341 not found."));
        while (1);
    }
    sensor.setATIME(99);
    sensor.setASTEP(999);
    sensor.setGain(currentGain);
    updateGainDisplay();

    // --- SD ---
    // SHARED_SPI uses SPI transactions — required to coexist with Adafruit libraries
    sdAvailable = sd.begin(SdSpiConfig(SD_CS, SHARED_SPI, SD_SCK_MHZ(4)));
    if (!sdAvailable) {
        Serial.println(F("SD init failed — result will display only, not saved."));
    }

    // --- InputManager ---
    inputMgr.init();

    enterState(CalState::MEASURING);
    Serial.println(F("Calibration sketch ready."));
}

// ---------------------------------------------------------------------------
void loop() {
    inputMgr.update();
    JoystickDir joy = inputMgr.getJoystickDirection();

    switch (calState) {

        // -------------------------------------------------------------------
        case CalState::MEASURING: {
            // Check button BEFORE the blocking sensor read so a press is never
            // delayed by a full integration cycle (~278ms, up to 4× with auto-gain).
            if (inputMgr.isButtonPressed(ButtonId::SELECT)) {
                enterState(CalState::LUX_ENTRY);
                break;
            }
            static unsigned long lastRead = 0;
            if (millis() - lastRead >= 500) {
                lastRead = millis();
                if (readSensor()) {
                    detectedLight = classifyLight(ch);
                    updateGainDisplay();
                    updateBars(ch);
                    updateLightTypeRow(detectedLight);
                }
            }
            break;
        }

        // -------------------------------------------------------------------
        case CalState::LUX_ENTRY: {
            bool changed = false;

            // Joystick UP/DOWN: change the highlighted digit
            if (joy == JoystickDir::UP) {
                luxDigits[luxCursor] = (luxDigits[luxCursor] == 9) ? 0 : luxDigits[luxCursor] + 1;
                changed = true;
            } else if (joy == JoystickDir::DOWN) {
                luxDigits[luxCursor] = (luxDigits[luxCursor] == 0) ? 9 : luxDigits[luxCursor] - 1;
                changed = true;
            }
            // Joystick LEFT/RIGHT: move cursor towards MSB / LSB
            else if (joy == JoystickDir::LEFT && luxCursor > 0) {
                luxCursor--;
                changed = true;
            } else if (joy == JoystickDir::RIGHT && luxCursor < 4) {
                luxCursor++;
                changed = true;
            }

            if (changed) drawLuxDigits();

            // Button A: confirm entry → compute
            if (inputMgr.isButtonPressed(ButtonId::SELECT)) {
                enterState(CalState::COMPUTING);
            }
            // Button B: cancel → back to live view
            if (inputMgr.isButtonPressed(ButtonId::BACK)) {
                // Reset bar/type state so MEASURING redraws cleanly
                for (uint8_t i = 0; i < 8; i++) prevBarH[i] = 0;
                prevLightType = 255;
                // Redraw the chart and type areas that were frozen
                tft.fillRect(0, CHART_TOP, SCREEN_W, CHART_H, ST77XX_BLACK);
                updateBars(ch);
                enterState(CalState::MEASURING);
            }
            break;
        }

        // -------------------------------------------------------------------
        case CalState::COMPUTING: {
            computeAndSave();
            enterState(CalState::RESULT);
            break;
        }

        // -------------------------------------------------------------------
        case CalState::RESULT: {
            // Button A: start a new calibration run
            if (inputMgr.isButtonPressed(ButtonId::SELECT)) {
                // Force full redraw of chart + type regions
                for (uint8_t i = 0; i < 8; i++) prevBarH[i] = 0;
                prevLightType = 255;
                tft.fillRect(0, CHART_TOP, SCREEN_W, CHART_H, ST77XX_BLACK);
                enterState(CalState::MEASURING);
            }
            break;
        }
    }
}
