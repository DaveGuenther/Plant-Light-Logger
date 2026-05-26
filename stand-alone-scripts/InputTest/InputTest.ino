/**
 * InputTest.ino
 * Tests all user inputs: 4 tactile buttons + analog joystick (X/Y axes).
 * Each input is shown as a labelled box on the TFT; the box fills with
 * colour when active.
 *
 * Buttons (INPUT_PULLUP, active LOW):
 *   A -> Pin 22   B -> Pin 23   VIEW -> Pin 24   SET -> Pin 25
 *
 * Joystick (analog, 0-1023):
 *   X axis -> A0   (< 200 = LEFT,  > 823 = RIGHT)
 *   Y axis -> A1   (< 200 = DOWN,  > 823 = UP)
 *   If directions are inverted for your joystick, swap the < / > signs below.
 *
 * Wiring: same as Spectrometer — see CLAUDE.md.
 *
 * Layout (160x128 landscape):
 *   y=0..15   header
 *   y=16..85  joystick D-pad cross
 *   y=86..127 four button boxes
 */

#include <TFT_eSPI.h>

// --- Pins ---
#define PIN_BTN_A     22
#define PIN_BTN_B     23
#define PIN_BTN_VIEW  24
#define PIN_BTN_SET   25
#define PIN_JOY_X     A0
#define PIN_JOY_Y     A1

// --- Joystick deflection thresholds ---
#define JOY_LOW   200
#define JOY_HIGH  823

// --- Button debounce ---
#define DEBOUNCE_MS  35

// --- Colors ---
#define COL_JOY  TFT_CYAN
#define COL_BTN  TFT_GREEN
#define COL_DIM  0x4208   // dark grey outline when inactive
#define COL_LBL  0x7BEF   // dim white label when inactive

TFT_eSPI tft;

// -------------------------------------------------------
// Box descriptor — position, size, label
// -------------------------------------------------------
struct Box { uint8_t x, y, w, h; const char* label; };

// D-pad cross centred at (80, 50).
// UP/DOWN arms are 26×14; LEFT/RIGHT arms are the same.
// Gap between arm edge and centre: ~18px each side horizontally, 8px vertically.
static const Box JOY_BOXES[4] = {
    { 67, 23, 26, 14, "UP" },   // [0] UP
    { 67, 63, 26, 14, "DN" },   // [1] DOWN
    { 20, 43, 26, 14, "LT" },   // [2] LEFT
    {114, 43, 26, 14, "RT" },   // [3] RIGHT
};

// Four button boxes spanning the bottom section, 40px slots each.
static const Box BTN_BOXES[4] = {
    {  3, 96, 34, 24, "A"    },
    { 43, 96, 34, 24, "B"    },
    { 83, 96, 34, 24, "VIEW" },
    {123, 96, 34, 24, "SET"  },
};

// -------------------------------------------------------
// State
// -------------------------------------------------------
static bool stableBtn[4]        = {};
static bool lastRawBtn[4]       = {};
static unsigned long debounceStart[4] = {};
static bool prevBtn[4]          = {};
static bool prevJoy[4]          = {};

// -------------------------------------------------------
// Draw one box.
// active=true  → filled with activeColor, black label.
// active=false → black fill, dim outline, dim label.
// -------------------------------------------------------
void drawBox(const Box& b, bool active, uint16_t activeColor) {
    tft.setTextSize(1);
    if (active) {
        tft.fillRect(b.x, b.y, b.w, b.h, activeColor);
        tft.setTextColor(TFT_BLACK);
    } else {
        tft.fillRect(b.x, b.y, b.w, b.h, TFT_BLACK);
        tft.drawRect(b.x, b.y, b.w, b.h, COL_DIM);
        tft.setTextColor(COL_LBL);
    }
    uint8_t labelW = strlen(b.label) * 6;
    tft.setCursor(b.x + (b.w - labelW) / 2, b.y + (b.h - 8) / 2);
    tft.print(b.label);
}

// -------------------------------------------------------
// Static chrome — drawn once at startup
// -------------------------------------------------------
void drawChrome() {
    tft.fillScreen(TFT_BLACK);

    // Header
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE);
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

// -------------------------------------------------------
void setup() {
    Serial.begin(115200);

    tft.init();
    tft.setRotation(1);
    drawChrome();

    const uint8_t btnPins[4] = {PIN_BTN_A, PIN_BTN_B, PIN_BTN_VIEW, PIN_BTN_SET};
    for (uint8_t i = 0; i < 4; i++) {
        pinMode(btnPins[i], INPUT_PULLUP);
    }

    Serial.println(F("Input test ready. Press buttons or move joystick."));
}

// -------------------------------------------------------
void loop() {
    const uint8_t btnPins[4] = {PIN_BTN_A, PIN_BTN_B, PIN_BTN_VIEW, PIN_BTN_SET};

    // --- Buttons: debounce, then redraw only on state change ---
    for (uint8_t i = 0; i < 4; i++) {
        bool raw = (digitalRead(btnPins[i]) == LOW);

        if (raw != lastRawBtn[i]) {
            lastRawBtn[i]      = raw;
            debounceStart[i]   = millis();
        }
        if ((millis() - debounceStart[i]) >= DEBOUNCE_MS) {
            stableBtn[i] = raw;
        }

        if (stableBtn[i] != prevBtn[i]) {
            prevBtn[i] = stableBtn[i];
            drawBox(BTN_BOXES[i], stableBtn[i], COL_BTN);
            Serial.print(BTN_BOXES[i].label);
            Serial.println(stableBtn[i] ? F(" pressed") : F(" released"));
        }
    }

    // --- Joystick: threshold comparison, redraw only on change ---
    // Analog thresholds create a natural deadband — no debounce needed.
    int xVal = analogRead(PIN_JOY_X);
    int yVal = analogRead(PIN_JOY_Y);

    bool joyNow[4] = {
        yVal > JOY_HIGH,   // [0] UP
        yVal < JOY_LOW,    // [1] DOWN
        xVal < JOY_LOW,    // [2] LEFT
        xVal > JOY_HIGH,   // [3] RIGHT
    };

    for (uint8_t i = 0; i < 4; i++) {
        if (joyNow[i] != prevJoy[i]) {
            prevJoy[i] = joyNow[i];
            drawBox(JOY_BOXES[i], joyNow[i], COL_JOY);
            if (joyNow[i]) {
                const char* names[4] = {"UP", "DOWN", "LEFT", "RIGHT"};
                Serial.print(F("Joy: "));
                Serial.println(names[i]);
            }
        }
    }
}
