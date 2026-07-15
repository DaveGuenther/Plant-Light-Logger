/**
 * SDTest.ino
 * Writes a sample CSV file to the SD card, then reads it back and
 * displays every line on the TFT screen.
 *
 * Wiring (15-pin side, 330Ω/680Ω voltage dividers on all 5V→TFT signal lines):
 *
 *   TFT signals (pins 3–8 on the module):
 *     Module pin 3  RST        → 330/680 divider → Mega D5
 *     Module pin 4  RS/DC      → 330/680 divider → Mega D6
 *     Module pin 5  SDA(MOSI)  → 330/680 divider → Mega 51 (HW SPI MOSI)
 *     Module pin 6  SCK        → 330/680 divider → Mega 52 (HW SPI SCK)
 *     Module pin 7  CS(TFT)    → 330/680 divider → Mega D7
 *     Module pin 8  BL         → Mega D44 via 68Ω resistor (current-limiting, not a divider)
 *
 *   SD card signals (pins 11–14 on the module):
 *     Module pin 11 SD_CS      → 330/680 divider → Mega D4
 *     Module pin 12 SD_MOSI    → 330/680 divider → Mega 51 (shared with TFT)
 *     Module pin 13 SD_MISO    → Mega 50 directly (3.3V output, no divider needed)
 *     Module pin 14 SD_CLK     → 330/680 divider → Mega 52 (shared with TFT)
 *
 *   Power:
 *     Module pin 1  GND        → Mega GND
 *     Module pin 2  VCC        → Mega 3.3V (module J1 SHORT for 3.3V mode)
 *                                  OR Mega 5V (module J1 OPEN for 5V mode)
 *
 * Voltage divider (per signal line, 5V→3.3V):
 *   Mega pin ──[ 330Ω ]──┬── Module pin
 *                        │
 *                      [680Ω]
 *                        │
 *                       GND
 *   Output = 5V × 680/(330+680) ≈ 3.37V  ✓
 *
 * Libraries required (install via Library Manager):
 *   - Adafruit ST7735 and ST7789 Library
 *   - Adafruit GFX Library
 *   SD library is built into the Arduino IDE.
 */

#include <SPI.h>
#include <SdFat.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

#define TFT_CS    7  // was 10
#define TFT_DC    6  // was 9  (Also RS)
#define TFT_RST   5  // was 8
#define TFT_BL    44  // backlight, via 68Ω resistor
#define SD_CS_PIN  4

#define FILENAME   "LOG.CSV"

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
SdFat32         sd;

// Sample data — five rows that look like real logger output
struct Row { uint32_t ms; float par; float dli; };
static const Row ROWS[] = {
    {     0,  0.0f, 0.000f },
    {  1000, 14.2f, 0.014f },
    {  2000, 27.8f, 0.042f },
    {  3000, 31.5f, 0.074f },
    {  4000, 29.1f, 0.103f },
};
static const uint8_t NUM_ROWS = sizeof(ROWS) / sizeof(ROWS[0]);

// -----------------------------------------------------------------------
// Status line — overwrites the same row on each call
void showStatus(const __FlashStringHelper* msg, uint16_t color = ST77XX_WHITE) {
    tft.fillRect(0, 20, 160, 10, ST77XX_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(color);
    tft.setCursor(2, 20);
    tft.print(msg);
}

void setup() {
    Serial.begin(115200);

    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);  // backlight on

    tft.initR(INITR_BLACKTAB);
    tft.setRotation(1);
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(2, 4);
    tft.print(F("SD TEST"));
    tft.drawFastHLine(0, 15, 160, 0x2945);

    // ---- Init SD --------------------------------------------------------
    showStatus(F("Init SD..."));

    if (!sd.begin(SdSpiConfig(SD_CS_PIN, SHARED_SPI, SD_SCK_MHZ(4)))) {
        showStatus(F("SD init FAILED"), ST77XX_RED);
        Serial.println(F("SD init failed. Check wiring."));
        return;
    }
    showStatus(F("SD OK"), ST77XX_GREEN);
    delay(400);

    // ---- Write ----------------------------------------------------------
    showStatus(F("Writing " FILENAME "..."));

    sd.remove(FILENAME);   // always start with a fresh file
    File32 f = sd.open(FILENAME, O_WRITE | O_CREAT | O_TRUNC);
    if (!f) {
        showStatus(F("Write open FAILED"), ST77XX_RED);
        return;
    }

    f.println(F("ms,par_umol,dli_mol"));
    for (uint8_t i = 0; i < NUM_ROWS; i++) {
        f.print(ROWS[i].ms);  f.print(',');
        f.print(ROWS[i].par, 1);  f.print(',');
        f.println(ROWS[i].dli, 3);
    }
    f.close();

    // ---- Read back and display on TFT ----------------------------------
    f = sd.open(FILENAME, O_READ);
    if (!f) {
        showStatus(F("Read open FAILED"), ST77XX_RED);
        return;
    }

    // Clear below the header bar and show the file label
    tft.fillRect(0, 16, 160, 112, ST77XX_BLACK);
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_YELLOW);
    tft.setCursor(2, 19);
    tft.print(FILENAME);
    tft.drawFastHLine(0, 29, 160, 0x2945);

    uint8_t lineY    = 32;
    char    lineBuf[32];
    uint8_t pos      = 0;
    bool    firstLine = true;

    while (f.available()) {
        char c = (char)f.read();
        if (c == '\n' || c == '\r') {
            if (pos > 0) {
                lineBuf[pos] = '\0';
                tft.setTextColor(firstLine ? ST77XX_YELLOW : ST77XX_WHITE);
                tft.setCursor(2, lineY);
                tft.print(lineBuf);
                lineY   += 10;
                pos      = 0;
                firstLine = false;
            }
        } else if (pos < (uint8_t)(sizeof(lineBuf) - 1)) {
            lineBuf[pos++] = c;
        }
    }
    // Flush any final line without a trailing newline
    if (pos > 0) {
        lineBuf[pos] = '\0';
        tft.setTextColor(ST77XX_WHITE);
        tft.setCursor(2, lineY);
        tft.print(lineBuf);
        lineY += 10;
    }
    f.close();

    tft.drawFastHLine(0, lineY + 1, 160, 0x2945);
    tft.setTextColor(ST77XX_GREEN);
    tft.setCursor(2, lineY + 4);
    tft.print(F("Done."));
}

void loop() {}
