# Plant Light Logger — Project Notes

## Hardware

- **Microcontroller:** Arduino Mega 2560
- **Display:** 1.8" ST7735 TFT, 128×160, SPI (blue PCB, "1.8 TFT MODULE / 8 SPI 128X160")
- **RTC:** DS3231 I2C real-time clock module

### Display Wiring (Mega 2560)

| TFT Pin | Mega Pin | Note |
|---------|----------|------|
| VCC     | 5V       |      |
| GND     | GND      |      |
| CS      | 7        |      |
| RST     | 5        |      |
| DC/AO   | 6        |      |
| DIN/MOSI| 51       | Hardware SPI |
| CLK/SCK | 52       | Hardware SPI |
| BL      | 44       | PWM-capable; via 68Ω resistor (~1.4mA) |

### Level Shifting (5V Mega → 3.3V display)

Use resistor voltage dividers on CS, RST, DC, MOSI, and CLK.
**Use low-value resistors:** 330Ω (top) + 680Ω (bottom) per signal line → ~3.37V output, ~220Ω source impedance.

**Do NOT use high-value resistors** (e.g. 3KΩ/5.6KΩ). The ~2KΩ Thevenin impedance combined with wire/pin capacitance creates an RC filter that rounds off SPI edges faster than the bit period, corrupting all pixel writes while allowing the slower init sequence to partially succeed. Symptom: display initializes (snow clears to white) but fillScreen has no effect.

---

## Display Library

**Use Adafruit ST7735 and ST7789 Library + Adafruit GFX Library** (install both via Library Manager).

### Sketch Setup

```cpp
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

#define TFT_CS   7
#define TFT_RST  5
#define TFT_DC   6
#define TFT_BL   44  // backlight, via 68Ω resistor

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
```

In `setup()`:
```cpp
pinMode(TFT_BL, OUTPUT);
digitalWrite(TFT_BL, HIGH);  // backlight on

tft.initR(INITR_BLACKTAB);
tft.setRotation(1);   // landscape
```

Colour constants use the `ST77XX_` prefix: `ST77XX_BLACK`, `ST77XX_WHITE`, `ST77XX_RED`, `ST77XX_GREEN`, `ST77XX_BLUE`, `ST77XX_YELLOW`, `ST77XX_CYAN`. Raw RGB565 hex values work directly with both `setTextColor()` and `fillRect()`.

---

## RTC Wiring (DS3231)

| DS3231 Pin | Mega Pin | Note |
|------------|----------|------|
| VCC        | 5V       | Module is 5V tolerant |
| GND        | GND      |      |
| SDA        | 20       | Shared I2C bus (also used by AS7341) |
| SCL        | 21       | Shared I2C bus (also used by AS7341) |

No voltage dividers needed — I2C is open-drain, and the DS3231 module's onboard pull-ups work fine at 5V. No address conflict with the AS7341 (DS3231 = `0x68`, AS7341 = `0x39`).

### Library

**Use RTClib by Adafruit** (install via Library Manager; accept the Adafruit BusIO dependency prompt).

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

