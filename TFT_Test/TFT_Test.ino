/**
 * TFT_Test.ino
 * Minimal display test — no sensor, just paint the screen and print text.
 * Cycles through RED → GREEN → BLUE every 2 seconds so it's obvious if working.
 *
 * Wiring (Mega 2560, hardware SPI, resistor dividers on signal lines):
 *   TFT RST -> Pin 8    TFT CS  -> Pin 10   TFT D/C -> Pin 9
 *   TFT BL  -> Pin 7    TFT DIN -> Pin 51   TFT CLK -> Pin 52
 *   TFT VCC -> 5V       TFT GND -> GND
 *
 * Requires TFT_eSPI library with User_Setup.h configured for ST7735 + Mega pins.
 */

#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();

void showColor(uint32_t bg, const char* label) {
    tft.fillScreen(bg);
    tft.setTextSize(2);
    tft.setTextColor(TFT_WHITE, bg);
    tft.setCursor(10, 50);
    tft.print(label);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE, bg);
    tft.setCursor(10, 80);
    tft.print("DRIVER: ST7735");
    tft.setCursor(10, 92);
    tft.print("TAB:    BLACKTAB");
    tft.setCursor(10, 104);
    tft.print("SPI:    HARDWARE");
}

void setup() {
    Serial.begin(115200);
    Serial.println(F("TFT test starting..."));

    tft.init();
    tft.setRotation(1);

    Serial.print(F("W: ")); Serial.println(tft.width());
    Serial.print(F("H: ")); Serial.println(tft.height());
    Serial.println(F("Init done. Starting color cycle."));
}

void loop() {
    showColor(TFT_RED,   "RED");
    delay(2000);
    showColor(TFT_GREEN, "GREEN");
    delay(2000);
    showColor(TFT_BLUE,  "BLUE");
    delay(2000);
}
