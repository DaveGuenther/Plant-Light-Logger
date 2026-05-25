# Plant Light Logger — Project Notes

## Hardware

- **Microcontroller:** Arduino Mega 2560
- **Display:** 1.8" ST7735 TFT, 128×160, SPI (blue PCB, "1.8 TFT MODULE / 8 SPI 128X160")

### Display Wiring (Mega 2560)

| TFT Pin | Mega Pin | Note |
|---------|----------|------|
| VCC     | 5V       |      |
| GND     | GND      |      |
| CS      | 10       |      |
| RST     | 8        |      |
| DC/AO   | 9        |      |
| DIN/MOSI| 51       | Hardware SPI |
| CLK/SCK | 52       | Hardware SPI |
| BL      | 7        | PWM backlight |

### Level Shifting (5V Mega → 3.3V display)

Use resistor voltage dividers on CS, RST, DC, MOSI, and CLK.
**Use low-value resistors:** 330Ω (top) + 680Ω (bottom) per signal line → ~3.37V output, ~220Ω source impedance.

**Do NOT use high-value resistors** (e.g. 3KΩ/5.6KΩ). The ~2KΩ Thevenin impedance combined with wire/pin capacitance creates an RC filter that rounds off SPI edges faster than the bit period, corrupting all pixel writes while allowing the slower init sequence to partially succeed. Symptom: display initializes (snow clears to white) but fillScreen has no effect.

---

## Display Library

**Use TFT_eSPI by Bodmer. Do NOT use Adafruit_GFX / Adafruit_ST7735.**

Adafruit_ST7735 was tested extensively and could not produce correct pixel output on this hardware regardless of init variant (GREENTAB, BLACKTAB, GREENTAB2, etc.) or SPI mode (software/hardware). Root cause was ultimately the resistor divider issue above, but TFT_eSPI was also adopted as it has better support for cheap ST7735 clones.

### TFT_eSPI User_Setup.h Configuration

The library's `User_Setup.h` (in the Arduino libraries folder) must be edited. Key settings:

```cpp
// Section 1 — Driver
#define ST7735_DRIVER        // enable this
//#define ILI9341_DRIVER     // disable the default

// Display dimensions
#define TFT_WIDTH  128
#define TFT_HEIGHT 160

// ST7735 tab variant — BLACKTAB works for this module (correct 128x160 dimensions)
// GREENTAB2 was also tried; both produce the same result for this module
#define ST7735_BLACKTAB

// Section 2 — Pins (Mega 2560 hardware SPI)
//#define TFT_MISO  50  // not connected
#define TFT_MOSI  51
#define TFT_SCLK  52
#define TFT_CS    10
#define TFT_DC     9
#define TFT_RST    8
#define TFT_BL     7
#define TFT_BACKLIGHT_ON HIGH

// Section 4 — SPI speed
#define SPI_FREQUENCY  8000000   // 8 MHz — safe with 330/680Ω dividers
```

### Known Display Quirk

A 1–2 pixel garbage strip appears at the top and left edges of the display regardless of tab variant or software settings. This is a physical characteristic of the LCD panel border (outside the controller's addressable RAM) and cannot be fixed in software. Keep all UI content at least 2 pixels away from the display edges.

---

## Sketch Notes

- Constructor: `TFT_eSPI tft = TFT_eSPI();` — no pin arguments needed, all pins come from User_Setup.h
- Init: `tft.init()` then `tft.setRotation(1)` for landscape
- Backlight is handled automatically by TFT_eSPI via TFT_BL/TFT_BACKLIGHT_ON defines
