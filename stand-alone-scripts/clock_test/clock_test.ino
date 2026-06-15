#include <Wire.h>
#include <RTClib.h>

RTC_DS3231 rtc;

void setup() {
    Serial.begin(115200);
    Wire.begin();
    if (!rtc.begin()) {
        Serial.println("DS3231 not found");
        while (1);
    }
    if (rtc.lostPower()) {
        rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
    Serial.println("RTC OK");
}

void loop() {
    DateTime now = rtc.now();
    Serial.print(now.year()); Serial.print('/');
    Serial.print(now.month()); Serial.print('/');
    Serial.print(now.day()); Serial.print(' ');
    Serial.print(now.hour()); Serial.print(':');
    Serial.println(now.minute());
    delay(1000);
}
