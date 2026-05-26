/**
 * Spectrometer.ino
 * Standalone spectral bar chart for AS7341 + ST7735 TFT.
 * Use this to validate hardware and assist with PAR calibration.
 *
 * Wiring:
 *   TFT RST  -> Pin 8    TFT CS  -> Pin 10   TFT D/C -> Pin 9
 *   TFT BL   -> Pin 7    TFT DIN -> Pin 51    TFT CLK -> Pin 52
 *   TFT VCC  -> 5V       TFT GND -> GND
 *   AS7341 SDA -> Pin 20  AS7341 SCL -> Pin 21
 *   AS7341 VCC -> 5V      AS7341 GND -> GND
 *
 * Display: 160px wide x 128px tall (landscape, setRotation(1))
 *
 * Auto-gain: after each read, if any channel >= 60000 gain steps down;
 * if peak < 1000 gain steps up. Up to 4 re-reads per cycle.
 * Current gain is shown in the header (top-right).
 *
 * Light classification: daylight balance threshold 0.15 catches overcast outdoor.
 * Window glass passes 415nm, so window-filtered daylight is spectrally identical to
 * outdoor — gain level distinguishes them (>=128x → Window Light, <128x → Sunlight).
 * Halogen vs warm LED split: ir_ratio = ch[7]/ch[6] > 0.75 → Halogen (smooth
 * blackbody); warm LED drops sharply at 680nm.
 * Fluorescent: includes ch[5] (590nm yellow phosphor) in detection fraction.
 */

#include <Wire.h>
#include <TFT_eSPI.h>
#include <Adafruit_AS7341.h>

// Pins are defined in TFT_eSPI User_Setup.h — no pin definitions needed here.

// --- Screen dimensions (landscape) ---
#define SCREEN_W  160
#define SCREEN_H  128

// --- Layout ---
//
//  y=0  ┌────────────────────────────────────────────┐
//       │  SPECTRAL PROFILE                  G:256x  │  16px header
//  y=16 ├────────────────────────────────────────────┤
//       │                                            │
//       │   [■] [■] [■] [■] [■] [■] [■] [■]         │  78px chart
//       │                                            │
//  y=95 ├────────────────────────────────────────────┤
//       │  415  445  480  515  555  590  630  680     │  12px wavelength labels
// y=108 ├────────────────────────────────────────────┤
//       │  LIGHT: Warm LED                           │  20px light type
// y=128 └────────────────────────────────────────────┘

#define GAIN_X     114   // x-start of gain label in header

#define CHART_TOP  17
#define CHART_H    78
#define CHART_BOT  (CHART_TOP + CHART_H)   // y=95

#define LABEL_Y   97
#define TYPE_Y   110

// 8 bars across 160px: 20px slot, 14px bar, 3px gap each side
#define BAR_SLOT  20
#define BAR_W     14
#define BAR_X(i)  ((i) * BAR_SLOT + 3)

// RGB565 colours per channel (violet → red)
static const uint16_t BAR_COLORS[8] = {
    0x780F,  // F1 415nm – purple
    0x001F,  // F2 445nm – blue
    0x055F,  // F3 480nm – blue-cyan
    0x07FF,  // F4 515nm – cyan
    0x07E0,  // F5 555nm – green
    0xFFE0,  // F6 590nm – yellow
    0xFC60,  // F7 630nm – orange
    0xF800,  // F8 680nm – red
};

static const char* WL_LABELS[8] = {
    "415","445","480","515","555","590","630","680"
};

TFT_eSPI tft;
Adafruit_AS7341 sensor;

static uint8_t prevBarH[8]    = {0};
static uint8_t prevLightType  = 255;
static as7341_gain_t currentGain     = AS7341_GAIN_256X;
static as7341_gain_t prevDisplayGain = (as7341_gain_t)255;  // sentinel forces first draw

// Gain multiplier strings for Serial CSV output (index matches as7341_gain_t enum)
static const char* GAIN_STRINGS[] = {
    "0.5","1","2","4","8","16","32","64","128","256","512"
};

// -------------------------------------------------------
// Light type classification
//
// Sunlight / Window Light: broad balanced spectrum (balance > 0.15).
//   0.15 threshold (not 0.22) catches overcast outdoor which has a natural
//   blue→red gradient. Window glass passes 415nm, so window-filtered daylight is
//   spectrally identical to outdoor. Gain level separates them:
//   >=128x → Window Light (dim), <128x → Sunlight (bright).
// Halogen: red-dominant AND smooth blackbody → 680nm ≈ 630nm (ir_ratio > 0.75).
// Warm LED: red-dominant but 680nm drops (≈40-60% of 630nm, ir_ratio < 0.75).
// Fluorescent: includes ch[5] (590nm yellow phosphor) in green fraction.
// -------------------------------------------------------
enum LightType : uint8_t {
    LT_SUNLIGHT = 0,
    LT_FILTERED_DAYLIGHT,   // natural light through window glass
    LT_COOL_LED,
    LT_WARM_LED,
    LT_HALOGEN,             // incandescent/halogen: smooth blackbody, 680nm ≈ 630nm
    LT_FLUORESCENT,
    LT_HPS_HID,
    LT_UNKNOWN
};

static const char* LIGHT_NAMES[] = {
    "Sunlight",
    "Window Light",
    "Cool LED",
    "Warm LED",
    "Halogen",
    "Fluorescent",
    "HPS / HID",
    "Unknown"
};

LightType classifyLight(const uint16_t* ch) {
    uint32_t total  = 0;
    uint16_t minCh  = 65535;
    uint16_t maxCh  = 0;

    for (uint8_t i = 0; i < 8; i++) {
        total += ch[i];
        if (ch[i] < minCh) minCh = ch[i];
        if (ch[i] > maxCh) maxCh = ch[i];
    }
    if (total == 0 || maxCh == 0) return LT_UNKNOWN;

    float blue    = (float)(ch[0] + ch[1] + ch[2]) / total;  // F1+F2+F3
    float green   = (float)(ch[3] + ch[4])          / total;  // F4+F5
    float red     = (float)(ch[6] + ch[7])          / total;  // F7+F8
    float balance = (float)minCh / maxCh;

    // Sunlight or window-filtered daylight: broad balanced spectrum.
    // Threshold 0.15 (not 0.22) to catch overcast outdoor with its natural
    // blue→red gradient (vegetation/ground reflects warm wavelengths).
    // Window glass passes 415nm (visible violet), so filtered daylight looks
    // spectrally identical to outdoor — gain level separates them.
    if (balance > 0.15f && blue > 0.15f && red > 0.12f) {
        return (currentGain >= AS7341_GAIN_128X) ? LT_FILTERED_DAYLIGHT : LT_SUNLIGHT;
    }

    // Artificial sources — checked only when one region clearly dominates.
    if (blue > 0.35f) return LT_COOL_LED;

    // Red-dominant: halogen has a smooth blackbody curve so 680nm ≈ 630nm (ir_ratio > 0.75);
    // warm LED peaks at 630nm and drops (680nm ≈ 40-60% of 630nm).
    if (red > 0.30f) {
        float ir_ratio = (ch[6] > 0) ? (float)ch[7] / ch[6] : 0.0f;
        return (ir_ratio > 0.75f) ? LT_HALOGEN : LT_WARM_LED;
    }

    if (green > 0.40f) return LT_HPS_HID;

    // Fluorescent: strong 590nm phosphor emission (ch[5]) plus moderate green/blue.
    float yellow = (float)ch[5] / total;
    if ((green + yellow) > 0.42f && blue > 0.14f && blue < 0.32f) return LT_FLUORESCENT;

    return LT_UNKNOWN;
}

// -------------------------------------------------------
// Gain label for TFT header
// -------------------------------------------------------
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

// Redrawn only when gain changes
void updateGainDisplay() {
    if (currentGain == prevDisplayGain) return;
    prevDisplayGain = currentGain;

    tft.fillRect(GAIN_X, 0, SCREEN_W - GAIN_X, 15, TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(0x7BEF);
    tft.setCursor(GAIN_X, 2);
    tft.print(gainLabel(currentGain));
}

// -------------------------------------------------------
// Auto-gain: one step up or down based on peak channel.
// Returns true if gain changed (caller re-reads with new gain).
// -------------------------------------------------------
bool autoAdjustGain(const uint16_t* ch) {
    uint16_t peak = 0;
    for (uint8_t i = 0; i < 8; i++) {
        if (ch[i] > peak) peak = ch[i];
    }
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

// -------------------------------------------------------
// Static chrome — drawn once on boot
// -------------------------------------------------------
void drawChrome() {
    tft.fillScreen(TFT_BLACK);

    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE);
    tft.setCursor(2, 2);
    tft.print(F("SPECTRAL PROFILE"));
    // gain label drawn by updateGainDisplay() after sensor init

    tft.drawFastHLine(0, CHART_TOP - 1, SCREEN_W, 0x2945);
    tft.drawFastHLine(0, CHART_BOT + 1, SCREEN_W, 0x2945);

    tft.setTextColor(0x7BEF);
    for (uint8_t i = 0; i < 8; i++) {
        // 3 chars × 6px = 18px wide; nudge left 2px to centre under 14px bar
        tft.setCursor(BAR_X(i) - 2, LABEL_Y);
        tft.print(WL_LABELS[i]);
    }

    tft.drawFastHLine(0, TYPE_Y - 2, SCREEN_W, 0x2945);
}

// -------------------------------------------------------
// Differential bar redraw — only pixels that changed
// -------------------------------------------------------
void updateBars(const uint16_t* ch) {
    uint16_t peak = 1;
    for (uint8_t i = 0; i < 8; i++) {
        if (ch[i] > peak) peak = ch[i];
    }

    for (uint8_t i = 0; i < 8; i++) {
        uint8_t newH   = (uint8_t)((uint32_t)ch[i] * CHART_H / peak);
        if (newH == prevBarH[i]) continue;

        uint8_t x      = BAR_X(i);
        uint8_t newTop = CHART_BOT - newH;
        uint8_t oldTop = CHART_BOT - prevBarH[i];

        if (newH > prevBarH[i]) {
            tft.fillRect(x, newTop, BAR_W, newH - prevBarH[i], BAR_COLORS[i]);
        } else {
            tft.fillRect(x, oldTop, BAR_W, prevBarH[i] - newH, TFT_BLACK);
        }

        prevBarH[i] = newH;
    }
}

// -------------------------------------------------------
// Light type label — redrawn only when classification changes
// -------------------------------------------------------
void updateLightType(LightType lt) {
    if ((uint8_t)lt == prevLightType) return;
    prevLightType = (uint8_t)lt;

    tft.fillRect(0, TYPE_Y, SCREEN_W, SCREEN_H - TYPE_Y, TFT_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(TFT_YELLOW);
    tft.setCursor(2, TYPE_Y);
    tft.print(F("LIGHT: "));
    tft.setTextColor(TFT_WHITE);
    tft.print(LIGHT_NAMES[lt]);
}

// -------------------------------------------------------
void setup() {
    Serial.begin(115200);

    tft.init();
    tft.setRotation(1);
    drawChrome();

    Wire.begin();
    if (!sensor.begin()) {
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_RED);
        tft.setTextSize(1);
        tft.setCursor(4, 50);
        tft.print(F("AS7341 NOT FOUND"));
        tft.setCursor(4, 62);
        tft.print(F("Check SDA(20)/SCL(21)"));
        Serial.println(F("AS7341 not found. Check SDA pin 20, SCL pin 21."));
        while (1);
    }

    // ATIME=99, ASTEP=999 -> ~278ms integration per read
    // Gain starts at 256x and is adjusted automatically each cycle
    sensor.setATIME(99);
    sensor.setASTEP(999);
    sensor.setGain(currentGain);
    updateGainDisplay();  // show initial gain immediately

    Serial.println(F("Ready. Serial output is CSV:"));
    Serial.println(F("F1(415),F2(445),F3(480),F4(515),F5(555),F6(590),F7(630),F8(680),gain_x"));
}

// -------------------------------------------------------
void loop() {
    static unsigned long lastUpdate = 0;
    if (millis() - lastUpdate < 500) return;
    lastUpdate = millis();

    uint16_t ch[8];

    // Auto-gain: read, check saturation/darkness, adjust one gain step and
    // re-read if needed. readAllChannels() waits for a full new integration,
    // so no manual delay is required after setGain().
    for (uint8_t attempt = 0; attempt < 4; attempt++) {
        if (!sensor.readAllChannels()) {
            Serial.println(F("Read failed"));
            return;
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

    // CSV to Serial — 8 channels + gain multiplier
    for (uint8_t i = 0; i < 8; i++) {
        Serial.print(ch[i]);
        Serial.print(F(","));
    }
    Serial.println(GAIN_STRINGS[(int)currentGain]);

    updateGainDisplay();
    updateBars(ch);
    updateLightType(classifyLight(ch));
}
