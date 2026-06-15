/**
 * TFT_Test.ino
 * Minimal display test — no sensor, just paint the screen and print text.
 * Cycles through RED → GREEN → BLUE every 2 seconds so it's obvious if working.
 * Backlight pulses 0->255->0 brightness via PWM during each color, confirmed
 * safe for continuous use at 13mA through the 68Ω series resistor.
 *
 * Wiring (Mega 2560, hardware SPI, 330/680Ω voltage dividers on signal lines):
 *   TFT RST -> Pin 5    TFT CS  -> Pin 7   TFT DC/RS  -> Pin 6
 *   TFT BL  -> Pin 44 (PWM, via 68Ω resistor)
 *   TFT SDA/MOSI/DIN -> Pin 51   TFT CLK/SCK -> Pin 52
 *   TFT VCC -> 3.3V     TFT GND -> GND
 *
 * Note: BL uses pin 44 (Timer5) instead of pin 3 (Timer3) because pin 3 sits
 * right next to the RST/DC/CS lines (5/6/7) on the Mega header — PWM
 * switching noise there coupled into those lines and corrupted the display.
 * Pin 44 is on the opposite header block, away from any TFT signal pins.
 *
 * Libraries: Adafruit GFX Library + Adafruit ST7735 and ST7789 Library
 */

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

#define TFT_CS   7  // was 10
#define TFT_DC   6  // was 9  (Also RS)
#define TFT_RST  5  // was 8
#define TFT_BL   44  // PWM backlight, via 68Ω resistor (was pin 3 - see note above)


Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

void showColor(uint16_t bg, const char* label) {
    tft.fillScreen(bg);
    tft.setTextSize(2);
    tft.setTextColor(ST77XX_WHITE, bg);
    tft.setCursor(10, 50);
    tft.print(label);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE, bg);
    tft.setCursor(10, 80);
    tft.print("DRIVER: ST7735");
    tft.setCursor(10, 92);
    tft.print("TAB:    BLACKTAB");
    tft.setCursor(10, 104);
    tft.print("SPI:    HARDWARE");
}

// Ramp the backlight 0->255->0 over ~2 seconds (255 steps * 2 * 4ms)
void pulseBacklight() {
    for (int b = 0; b <= 255; b++) {
        analogWrite(TFT_BL, b);
        delay(4);
    }
    for (int b = 255; b >= 0; b--) {
        analogWrite(TFT_BL, b);
        delay(4);
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println(F("TFT test starting..."));

    pinMode(TFT_BL, OUTPUT);

    tft.initR(INITR_BLACKTAB);
    tft.setRotation(1);

    Serial.print(F("W: ")); Serial.println(tft.width());
    Serial.print(F("H: ")); Serial.println(tft.height());
    Serial.println(F("Init done. Starting color cycle."));
}

void loop() {
    showColor(ST77XX_RED,   "RED");
    pulseBacklight();
    showColor(ST77XX_GREEN, "GREEN");
    pulseBacklight();
    showColor(ST77XX_BLUE,  "BLUE");
    pulseBacklight();
}
