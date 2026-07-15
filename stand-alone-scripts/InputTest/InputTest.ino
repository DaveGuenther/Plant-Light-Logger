/**
 * InputTest.ino
 * Tests all user inputs: 4 tactile buttons + analog joystick (X/Y axes).
 * Each input is shown as a labelled box on the TFT; the box fills with
 * colour when active.
 *
 * Uses InputManager (../../lib/InputManager.h) for debouncing and joystick
 * normalisation — no raw pin logic here.
 *
 * Buttons (INPUT_PULLUP, active LOW):
 *   SELECT -> Pin 22   BACK -> Pin 23   VIEW -> Pin 24   SETTINGS -> Pin 25
 *
 * Joystick (analog, 0–1023):
 *   X axis -> A0   (< 200 = LEFT,  > 823 = RIGHT)
 *   Y axis -> A1   (< 200 = DOWN,  > 823 = UP)
 *   If directions are inverted for your joystick, swap the outer pot terminals
 *   (GND ↔ 5V) for that axis — do not change the thresholds in InputManager.
 *
 * Wiring (Mega 2560, hardware SPI, 330/680Ω voltage dividers on signal lines):
 *   TFT RST -> Pin 5    TFT CS  -> Pin 7    TFT DC  -> Pin 6
 *   TFT BL  -> Pin 44 (via 68Ω resistor)    TFT DIN -> Pin 51   TFT CLK -> Pin 52
 *   TFT VCC -> 3.3V     TFT GND -> GND
 *
 * TFT layout (160×128 landscape):
 *   y=0..15   header
 *   y=16..85  joystick D-pad cross
 *   y=86..127 four button boxes
 *
 * Serial output (115200 baud):
 *   • "A pressed" / "A released"          — button state changes
 *   • "Joy: UP" / "DOWN" / "LEFT" / "RIGHT" — rate-limited joystick ticks
 *     (fires once on deflection, then repeats every 250 ms while held)
 *
 * Libraries: Adafruit GFX Library + Adafruit ST7735 and ST7789 Library
 */

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include "InputManager.h"

#define TFT_CS   7  // was 10
#define TFT_DC   6  // was 9  (Also RS)
#define TFT_RST  5  // was 8
#define TFT_BL   44  // backlight, via 68Ω resistor

// --- Colors ---
#define COL_JOY  ST77XX_CYAN
#define COL_BTN  ST77XX_GREEN
#define COL_DIM  0x4208   // dark grey outline when inactive
#define COL_LBL  0x7BEF   // dim white label when inactive

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
InputManager inputMgr;

// ---------------------------------------------------------------------------
// Box descriptor — screen position, size, label
// ---------------------------------------------------------------------------
struct Box { uint8_t x, y, w, h; const char* label; };

// D-pad cross centred at approximately (80, 50).
// UP/DOWN arms: 26×14 px.  LEFT/RIGHT arms: same.
static const Box JOY_BOXES[4] = {
    { 67, 23, 26, 14, "UP" },   // [0] UP
    { 67, 63, 26, 14, "DN" },   // [1] DOWN
    { 20, 43, 26, 14, "LT" },   // [2] LEFT
    {114, 43, 26, 14, "RT" },   // [3] RIGHT
};

// Four button boxes spanning the bottom section, ~40 px slots each.
static const Box BTN_BOXES[4] = {
    {  3, 96, 34, 24, "SEL"  },
    { 43, 96, 34, 24, "BACK" },
    { 83, 96, 34, 24, "VIEW" },
    {123, 96, 34, 24, "STGS" },
};

// ---------------------------------------------------------------------------
// Persistent display state (redraw only on change)
// ---------------------------------------------------------------------------
static bool prevJoy[4]  = {};  // last rendered joystick direction flags
static bool prevHeld[4] = {};  // last rendered button hold states

// ---------------------------------------------------------------------------
// Draw one box.
//   active = true  → filled with activeColor, black label
//   active = false → black fill, dim outline, dim label
// ---------------------------------------------------------------------------
void drawBox(const Box& b, bool active, uint16_t activeColor) {
    tft.setTextSize(1);
    if (active) {
        tft.fillRect(b.x, b.y, b.w, b.h, activeColor);
        tft.setTextColor(ST77XX_BLACK);
    } else {
        tft.fillRect(b.x, b.y, b.w, b.h, ST77XX_BLACK);
        tft.drawRect(b.x, b.y, b.w, b.h, COL_DIM);
        tft.setTextColor(COL_LBL);
    }
    uint8_t labelW = strlen(b.label) * 6;
    tft.setCursor(b.x + (b.w - labelW) / 2, b.y + (b.h - 8) / 2);
    tft.print(b.label);
}

// ---------------------------------------------------------------------------
// Static chrome — drawn once at startup, never re-cleared
// ---------------------------------------------------------------------------
void drawChrome() {
    tft.fillScreen(ST77XX_BLACK);

    // Header
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(50, 4);
    tft.print(F("INPUT TEST"));
    tft.drawFastHLine(0, 15, 160, 0x2945);

    // Joystick section label
    tft.setTextColor(COL_LBL);
    tft.setCursor(56, 18);
    tft.print(F("JOYSTICK"));

    // Divider between sections
    tft.drawFastHLine(0, 86, 160, 0x2945);

    // Button section label
    tft.setCursor(59, 88);
    tft.print(F("BUTTONS"));

    // All boxes in inactive state
    for (uint8_t i = 0; i < 4; i++) {
        drawBox(JOY_BOXES[i], false, COL_JOY);
        drawBox(BTN_BOXES[i], false, COL_BTN);
    }
}

// ---------------------------------------------------------------------------
void setup() {
    Serial.begin(115200);

    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);  // backlight on

    tft.initR(INITR_BLACKTAB);
    tft.setRotation(1);  // landscape, USB connector on left
    drawChrome();

    inputMgr.init();     // configures INPUT_PULLUP on all button pins

    Serial.println(F("Input test ready. Press buttons or move joystick."));
}

// ---------------------------------------------------------------------------
void loop() {
    // Must be called first — latches fresh hardware state for this cycle.
    inputMgr.update();

    // -----------------------------------------------------------------------
    // Buttons
    // isButtonHeld()  → current debounced state, used to drive the display.
    // Tracking prevHeld[] means we only redraw and print on transitions,
    // matching the original behaviour (no flood on hold).
    // -----------------------------------------------------------------------
    for (uint8_t i = 0; i < 4; i++) {
        bool held = inputMgr.isButtonHeld(static_cast<ButtonId>(i));

        if (held != prevHeld[i]) {
            prevHeld[i] = held;
            drawBox(BTN_BOXES[i], held, COL_BTN);
            Serial.print(BTN_BOXES[i].label);
            Serial.println(held ? F(" pressed") : F(" released"));
        }
    }

    // -----------------------------------------------------------------------
    // Joystick display
    // getRawJoystickDir() returns the instantaneous direction so the D-pad
    // boxes stay lit continuously while the stick is deflected.
    // -----------------------------------------------------------------------
    JoystickDir rawDir = inputMgr.getRawJoystickDir();
    bool joyNow[4] = {
        rawDir == JoystickDir::UP,
        rawDir == JoystickDir::DOWN,
        rawDir == JoystickDir::LEFT,
        rawDir == JoystickDir::RIGHT,
    };
    for (uint8_t i = 0; i < 4; i++) {
        if (joyNow[i] != prevJoy[i]) {
            prevJoy[i] = joyNow[i];
            drawBox(JOY_BOXES[i], joyNow[i], COL_JOY);
        }
    }

    // -----------------------------------------------------------------------
    // Joystick Serial ticks
    // getJoystickDirection() emits once on deflection then auto-repeats every
    // 250 ms — the same rate that UIManager will use for menu scrolling.
    // -----------------------------------------------------------------------
    JoystickDir tick = inputMgr.getJoystickDirection();
    if (tick != JoystickDir::NONE) {
        Serial.print(F("Joy: "));
        switch (tick) {
            case JoystickDir::UP:    Serial.println(F("UP"));    break;
            case JoystickDir::DOWN:  Serial.println(F("DOWN"));  break;
            case JoystickDir::LEFT:  Serial.println(F("LEFT"));  break;
            case JoystickDir::RIGHT: Serial.println(F("RIGHT")); break;
            default: break;
        }
    }
}
