/**
 * Minimaler ESP32-Test: SSD1306 128x64 per I2C (4 Pol).
 *
 * Verdrahtung wie timer_esp32:
 *   VCC -> 3V3, GND -> GND, SDA -> GPIO 21, SCL -> GPIO 22
 *
 * Board: ESP32 Dev Module, FQBN z. B. esp32:esp32:esp32
 * Bibliotheken: Adafruit SSD1306, Adafruit GFX, Adafruit BusIO
 *
 * Schwarzes Display: SSD1306_ADDR auf 0x3D setzen.
 * Modul 128x32: SCREEN_HEIGHT auf 32 und display(..., 32, ...) anpassen.
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define I2C_SDA 21
#define I2C_SCL 22
#define SSD1306_ADDR 0x3D

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

void setup() {
  Serial.begin(115200);
  delay(200);

  Wire.begin(I2C_SDA, I2C_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SSD1306_ADDR)) {
    Serial.println(F("SSD1306: begin fehlgeschlagen (Kabel / Adresse 0x3C vs 0x3D)"));
    for (;;)
      delay(1000);
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(F("Display OK"));
  display.println(F("ESP32 I2C"));
  display.print(F("SDA "));
  display.print(I2C_SDA);
  display.print(F(" SCL "));
  display.println(I2C_SCL);
  display.println(F("Adr 0x3C (sonst 0x3D)"));
  display.display();

  Serial.println(F("SSD1306: OK"));
}

void loop() {
  delay(5000);
}
