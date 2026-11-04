// rfmon — каркас RF Monitor для ESP32-DIV v2 (ESP32-S3, ILI9341 320x240).
// Этап 1: дисплей, статус-бар с батареей IP5306, рабочий стол модулей, лог в USB-CDC.
#include <Arduino.h>
#include <Wire.h>
#include <TFT_eSPI.h>

#define BL_PIN 7
#define IP5306_ADDR 0x75
#define SB_H 17

TFT_eSPI tft;
static int  g_pct = -1;
static bool g_chg = false;
static bool g_full = true;

static uint8_t i2cReg(uint8_t reg) {
  Wire.beginTransmission(IP5306_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  if (Wire.requestFrom(IP5306_ADDR, 1) != 1) return 0xFF;
  return Wire.read();
}

static void readBattery() {
  uint8_t v = i2cReg(0x78);
  int p = -1;
  if (v != 0xFF) {
    switch (v >> 4) {
      case 0xE: p = 25; break;
      case 0xC: p = 50; break;
      case 0x8: p = 75; break;
      case 0x0: p = 100; break;
      default:  p = 0; break;
    }
  }
  uint8_t s70 = i2cReg(0x70);
  bool chg = (s70 != 0xFF) && ((s70 >> 3) & 1);
  if (p != g_pct || chg != g_chg) g_full = true;
  g_pct = p;
  g_chg = chg;
}

static void drawBattery(int x, int y) {
  const int w = 24, h = 11;
  tft.drawRect(x, y, w, h, TFT_LIGHTGREY);
  tft.fillRect(x + w, y + 3, 2, 5, TFT_LIGHTGREY);
  int p = g_pct < 0 ? 0 : g_pct;
  uint16_t col = p > 50 ? TFT_GREEN : p > 20 ? TFT_YELLOW : TFT_RED;
  int fill = (w - 4) * p / 100;
  if (fill > 0) tft.fillRect(x + 2, y + 2, fill, h - 4, col);
  if (g_chg) {  // молния
    int cx = x + w / 2, cy = y + 1;
    tft.fillTriangle(cx + 1, cy, cx - 3, cy + 5, cx + 1, cy + 5, TFT_WHITE);
    tft.fillTriangle(cx - 1, cy + 4, cx + 3, cy + 9, cx - 1, cy + 9, TFT_WHITE);
  }
}

static void drawStatusBar() {
  tft.fillRect(0, 0, tft.width(), SB_H, 0x0861);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, 0x0861);
  tft.drawString("RF MON", 5, 4, 1);
  tft.setTextColor(TFT_LIGHTGREY, 0x0861);
  tft.drawString(g_chg ? "CHG" : "BAT", 64, 4, 1);

  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", g_pct < 0 ? 0 : g_pct);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(g_pct > 50 ? TFT_GREEN : g_pct > 20 ? TFT_YELLOW : TFT_RED, 0x0861);
  tft.drawString(buf, 288, 4, 1);
  drawBattery(292, 3);
  tft.setTextDatum(TL_DATUM);
}

static const char* MODULES[] = {
  "SUB-GHz   281-928 MHz  CC1101",
  "2.4 GHz   126 ch        NRF24",
  "Bluetooth scan  BLE+CLS",
  "Wi-Fi scan  AP/RSSI",
  "IR  recv/send",
  "Settings",
};
static const int N_MODULES = 6;
static int g_sel = 0;

static void drawDesktop() {
  tft.fillRect(0, SB_H, tft.width(), tft.height() - SB_H, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("MODULES", 6, SB_H + 4, 2);
  for (int i = 0; i < N_MODULES; i++) {
    int y = SB_H + 26 + i * 26;
    bool sel = (i == g_sel);
    if (sel) tft.fillRect(4, y - 3, tft.width() - 8, 22, 0x0A52);
    tft.drawRect(4, y - 3, tft.width() - 8, 22, sel ? TFT_CYAN : 0x18E3);
    tft.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, sel ? 0x0A52 : TFT_BLACK);
    tft.drawString(MODULES[i], 10, y, 1);
  }
  tft.fillRect(0, tft.height() - 18, tft.width(), 18, 0x0861);
  tft.setTextColor(TFT_LIGHTGREY, 0x0861);
  tft.drawString("UP/DOWN  OK=open   MENU", 6, tft.height() - 14, 1);
}

void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println();
  Serial.println("=== rfmon stage1 ===");

  pinMode(BL_PIN, OUTPUT);
  digitalWrite(BL_PIN, HIGH);

  tft.init();
  tft.setRotation(1);          // 320x240 landscape
  tft.fillScreen(TFT_BLACK);
  Serial.printf("TFT init: %dx%d\n", tft.width(), tft.height());

  Wire.begin(8, 9, 100000);
  readBattery();
  Serial.printf("battery: %d%% charging=%d\n", g_pct, (int)g_chg);

  drawStatusBar();
  drawDesktop();

  // проверка чтения с панели: рисуем пиксель и читаем его назад
  tft.drawPixel(300, 200, TFT_MAGENTA);
  uint16_t rb = tft.readPixel(300, 200);
  Serial.printf("pixel write=0x%04X read=0x%04X %s\n", TFT_MAGENTA, rb,
                (rb == TFT_MAGENTA) ? "OK" : "MISMATCH");
  Serial.println("=== ready ===");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last > 2000) {
    last = millis();
    int prevPct = g_pct;
    bool prevChg = g_chg;
    readBattery();
    if (g_pct != prevPct || g_chg != prevChg) {
      drawStatusBar();
      Serial.printf("battery update: %d%% chg=%d\n", g_pct, (int)g_chg);
    }
  }
}
