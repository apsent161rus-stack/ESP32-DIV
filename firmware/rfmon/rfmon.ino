// rfmon — RF Monitor для ESP32-DIV v2 (ESP32-S3, ILI9341 320x240, развёрнут на 180°).
// Экраны: 0 рабочий стол, 1 панорама sub-GHz (CC1101), 2 скан 2.4 ГГц (3x nRF24).
#include <Arduino.h>
#include <Wire.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <math.h>

#define BL_PIN      7
#define IP5306      0x75
#define PCF8574     0x20
#define SB_H        17
#define CC_CS        5
#define RF_SCK      12
#define RF_MISO     13
#define RF_MOSI     11

static const int NRF_CS[3] = {4, 48, 21};
static const int NRF_CE[3] = {15, 47, 14};

TFT_eSPI tft;

static int  g_pct = -1;
static bool g_chg = false;
static int  g_view = 0;            // 0 desktop, 1 sub-GHz, 2 2.4GHz
static bool g_needFrame = true;

static uint8_t i2cReg(uint8_t dev, uint8_t reg) {
  Wire.beginTransmission(dev);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0xFF;
  if (Wire.requestFrom(dev, 1) != 1) return 0xFF;
  return Wire.read();
}

static void readBattery() {
  uint8_t v = i2cReg(IP5306, 0x78);
  int p = -1;
  if (v != 0xFF) {
    switch (v >> 4) {
      case 0xE: p = 25;  break;
      case 0xC: p = 50;  break;
      case 0x8: p = 75;  break;
      case 0x0: p = 100; break;
      default:  p = 0;   break;
    }
  }
  uint8_t s70 = i2cReg(IP5306, 0x70);
  g_chg = (s70 != 0xFF) && ((s70 >> 3) & 1);
  g_pct = p;
}

static uint8_t buttons() {
  Wire.beginTransmission(PCF8574);
  Wire.write(0xFF);                 // все линии в режим входа
  if (Wire.endTransmission() != 0) return 0xFF;
  delayMicroseconds(200);
  if (Wire.requestFrom(PCF8574, 1) != 1) return 0xFF;
  return Wire.read();
}

/* ---------------- CC1101 ---------------- */
static void ccWrite(uint8_t addr, uint8_t val) {
  delayMicroseconds(2);
  digitalWrite(CC_CS, LOW);
  delayMicroseconds(2);
  SPI.transfer(addr & 0x3F);
  SPI.transfer(val);
  delayMicroseconds(2);
  digitalWrite(CC_CS, HIGH);
  delayMicroseconds(2);
}
static uint8_t ccReadB(uint8_t addr, bool burst) {
  delayMicroseconds(2);
  digitalWrite(CC_CS, LOW);
  delayMicroseconds(2);
  SPI.transfer((burst ? 0xC0 : 0x80) | (addr & 0x3F));
  uint8_t v = SPI.transfer(0x00);
  delayMicroseconds(2);
  digitalWrite(CC_CS, HIGH);
  delayMicroseconds(2);
  return v;
}
static void ccStrobe(uint8_t cmd) {
  delayMicroseconds(2);
  digitalWrite(CC_CS, LOW);
  delayMicroseconds(2);
  SPI.transfer(cmd);
  delayMicroseconds(2);
  digitalWrite(CC_CS, HIGH);
  delayMicroseconds(2);
}
static void ccSetFreq(float mhz) {
  uint32_t f = (uint32_t)(mhz * 65536.0f / 26.0f + 0.5f);
  ccWrite(0x0D, (f >> 16) & 0xFF);
  ccWrite(0x0E, (f >> 8) & 0xFF);
  ccWrite(0x0F, f & 0xFF);
}
static int ccRssiDbm() {
  uint8_t r = ccReadB(0x34, true);
  float sign = (r >= 128) ? (float)r - 256.0f : (float)r;
  return (int)(sign / 2.0f - 74.0f + 0.5f);
}
static void ccReset() {
  pinMode(CC_CS, OUTPUT);
  digitalWrite(CC_CS, HIGH);
  ccStrobe(0x30);
  delay(10);
}

static const uint8_t CC_REGS[][2] = {
    {0x00,0x0B},{0x02,0x0D},{0x03,0x07},{0x04,0xD3},{0x05,0x91},{0x06,0xFF},
    {0x07,0x04},{0x08,0x32},{0x0A,0x00},{0x0B,0x06},{0x0C,0x00},
    {0x15,0xF5},{0x16,0x83},{0x17,0x13},{0x18,0x22},{0x19,0xF8},{0x1A,0x15},
    {0x1B,0x07},{0x1C,0x30},{0x1D,0x18},{0x1E,0x16},{0x1F,0x6C},
    {0x20,0x03},{0x21,0x40},{0x22,0x91},{0x23,0xFB},{0x24,0x56},{0x25,0x10},
    {0x26,0xE9},{0x27,0x2A},{0x28,0x00},{0x29,0x1F},{0x2C,0x81},{0x2D,0x35},{0x2E,0x09},
  };

static void ccConfig() {
  for (auto& r : CC_REGS) ccWrite(r[0], r[1]);
  ccWrite(0x3E, 0x8E);
  ccSetFreq(433.92f);
  ccStrobe(0x36);
}

static int ccRxRssi(float mhz) {
  static int lastGood = -100;
  ccStrobe(0x36);                        // SIDLE
  delayMicroseconds(200);
  ccSetFreq(mhz);
  delayMicroseconds(200);
  ccStrobe(0x34);                        // SRX (авто-калибровка ~0.7 мс)
  delayMicroseconds(900);
  uint8_t raw = ccReadB(0x34, true);
  ccStrobe(0x36);
  if (raw == 0x80 || raw == 0xFF || raw == 0x00) return lastGood;
  int r = (raw >= 128) ? ((int)raw - 256) / 2 - 74 : (int)raw / 2 - 74;
  if (r < -115) return lastGood;         // мусор
  lastGood = r;
  return r;
}

static bool ccInit() {
  ccReset();
  delay(20);
  ccConfig();
  delay(60);
  uint8_t v = 0;
  for (int k = 0; k < 10; k++) { v = ccReadB(0x31, true); if (v == 0x14) break; delay(20); }
  return v == 0x14;
}

/* ---------------- nRF24 ---------------- */
static void nrfReg(uint8_t csn, uint8_t reg, bool write, uint8_t val) {
  for (int i = 0; i < 3; i++) digitalWrite(NRF_CS[i], HIGH);
  digitalWrite(CC_CS, HIGH);
  digitalWrite(csn, LOW);
  SPI.transfer((write ? 0x20 : 0x00) | (reg & 0x1F));
  if (write) SPI.transfer(val); else SPI.transfer(0xFF);
  digitalWrite(csn, HIGH);
}
static uint8_t nrfRead(uint8_t csn, uint8_t reg) {
  for (int i = 0; i < 3; i++) digitalWrite(NRF_CS[i], HIGH);
  digitalWrite(CC_CS, HIGH);
  digitalWrite(csn, LOW);
  SPI.transfer(reg & 0x1F);
  uint8_t v = SPI.transfer(0xFF);
  digitalWrite(csn, HIGH);
  return v;
}
static void nrfInit(int i) {
  pinMode(NRF_CE[i], OUTPUT);
  digitalWrite(NRF_CE[i], LOW);
  pinMode(NRF_CS[i], OUTPUT);
  digitalWrite(NRF_CS[i], HIGH);
  nrfReg(NRF_CS[i], 0x01, true, 0x00);
  nrfReg(NRF_CS[i], 0x02, true, 0x01);                 // EN_RXADDR: пайп 0
  const uint8_t addr[5] = {0xE7, 0xE7, 0xE7, 0xE7, 0xE7};
  for (int b = 0; b < 5; b++) nrfReg(NRF_CS[i], 0x0A + b, true, addr[b]);
  nrfReg(NRF_CS[i], 0x11, true, 32);                   // RX_PW_P0
  nrfReg(NRF_CS[i], 0x03, true, 0x03);
  nrfReg(NRF_CS[i], 0x04, true, 0x00);
  nrfReg(NRF_CS[i], 0x05, true, 76);
  nrfReg(NRF_CS[i], 0x06, true, 0x0F);
  nrfReg(NRF_CS[i], 0x00, true, 0x03);
  delay(2);
}
static bool nrfRpd(int i) { return (nrfRead(NRF_CS[i], 0x09) & 1) != 0; }

/* ---------------- отрисовка ---------------- */
static void drawBatteryIcon(int x, int y) {
  const int w = 24, h = 11;
  tft.drawRect(x, y, w, h, TFT_LIGHTGREY);
  tft.fillRect(x + w, y + 3, 2, 5, TFT_LIGHTGREY);
  int p = g_pct < 0 ? 0 : g_pct;
  uint16_t col = p > 50 ? TFT_GREEN : p > 20 ? TFT_YELLOW : TFT_RED;
  int fill = (w - 4) * p / 100;
  if (fill > 0) tft.fillRect(x + 2, y + 2, fill, h - 4, col);
  if (g_chg) {
    int cx = x + w / 2, cy = y + 1;
    tft.fillTriangle(cx + 1, cy, cx - 3, cy + 5, cx + 1, cy + 5, TFT_WHITE);
    tft.fillTriangle(cx - 1, cy + 4, cx + 3, cy + 9, cx - 1, cy + 9, TFT_WHITE);
  }
}

static void drawStatusBar(const char* left, const char* right) {
  
  tft.fillRect(0, 0, tft.width(), SB_H, 0x0861);
  
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, 0x0861);
  tft.drawString(left, 5, 4, 1);
  tft.setTextColor(TFT_LIGHTGREY, 0x0861);
  tft.drawString(right, 82, 4, 1);
  
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", g_pct < 0 ? 0 : g_pct);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(g_pct > 50 ? TFT_GREEN : g_pct > 20 ? TFT_YELLOW : TFT_RED, 0x0861);
  tft.drawString(buf, 288, 4, 1);
  drawBatteryIcon(292, 3);
  
  tft.setTextDatum(TL_DATUM);
}

static const char* MODULES[] = {
  "SUB-GHz   281-928 MHz  CC1101",
  "2.4 GHz   126 ch        3xNRF",
  "Bluetooth scan  BLE+CLS",
  "Wi-Fi scan  AP/RSSI",
  "IR  recv/send",
  "Settings",
};
#define N_MOD 6
static int g_sel = 0;

static void footer(const char* hint) {
  tft.fillRect(0, tft.height() - 18, tft.width(), 18, 0x0861);
  tft.setTextColor(TFT_LIGHTGREY, 0x0861);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(hint, 6, tft.height() - 14, 1);
}

static const char* MOD_T[6] = {"SUB-GHz", "2.4 GHz", "BT scan", "Wi-Fi", "IR", "Settings"};
static const char* MOD_S[6] = {"CC1101 300-928", "3x NRF24 126ch", "BLE + classic", "AP / RSSI", "recv / send", "bright, sleep"};
static const char MOD_IC[6] = {'S', '2', 'B', 'W', 'I', 'G'};

static void drawDesktop() {
  tft.fillRect(0, SB_H, tft.width(), tft.height() - SB_H, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("MODULES", 6, SB_H + 3, 1);

  const int x0 = 4, y0 = SB_H + 16, tw = 154, th = 54, gx = 4, gy = 4;
  for (int i = 0; i < 6; i++) {
    int col = i % 2, row = i / 2;
    int x = x0 + col * (tw + gx), y = y0 + row * (th + gy);
    bool sel = (i == g_sel);
    uint16_t bg = sel ? 0x0A52 : 0x0841;
    tft.fillRoundRect(x, y, tw, th, 3, bg);
    tft.drawRoundRect(x, y, tw, th, 3, sel ? TFT_CYAN : 0x18E3);
    if (sel) tft.drawRoundRect(x - 1, y - 1, tw + 2, th + 2, 4, TFT_CYAN);
    // иконка
    tft.fillRoundRect(x + 6, y + 8, 24, 24, 3, sel ? TFT_CYAN : 0x18E3);
    tft.setTextColor(sel ? 0x0420 : TFT_CYAN, sel ? TFT_CYAN : 0x18E3);
    char ic[2] = {MOD_IC[i], 0};
    tft.drawString(ic, x + 14, y + 15, 2);
    // текст
    tft.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, bg);
    tft.drawString(MOD_T[i], x + 36, y + 10, 1);
    tft.setTextColor(sel ? 0x9FF4 : 0x5B72, bg);
    tft.drawString(MOD_S[i], x + 36, y + 26, 1);
    if (i == 0 || i == 1) {   // модуль реально отвечает
      tft.fillCircle(x + tw - 7, y + 7, 2, TFT_GREEN);
    }
  }
  footer("UP/DOWN  OK=open  MENU");
}

#define SPEC_TOP  (SB_H + 16)
#define SPEC_BOT  (tft.height() - 34)
static const float SWEEP_LO = 300.0f, SWEEP_HI = 928.0f;

static int dbmToY(int dbm) {
  const int lo = -100, hi = -30;
  if (dbm < lo) dbm = lo;
  if (dbm > hi) dbm = hi;
  return SPEC_BOT - (dbm - lo) * (SPEC_BOT - SPEC_TOP) / (hi - lo);
}

static void drawPanoramaFrame() {
  
  tft.fillRect(0, SB_H, tft.width(), tft.height() - SB_H, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("SUB-GHz PANORAMA", 6, SB_H + 3, 1);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("300-928 MHz CC1101", tft.width() - 6, SB_H + 3, 1);
  tft.setTextDatum(TL_DATUM);
  for (int i = 1; i < 4; i++) {
  
    int y = SPEC_TOP + i * (SPEC_BOT - SPEC_TOP) / 4;
    tft.drawFastHLine(0, y, tft.width(), 0x10A2);
  }
  footer("LEFT/RIGHT view");
  
}

static void sweepSubGHz() {
  const int N = 160;                        // 2 пикселя на точку — вдвое быстрее
  static uint8_t lvl[160];
  int peak = -127; float peakF = 0; long sum = 0; int mn = 127, mx = -127;

  for (int i = 0; i < N; i++) {
    float f = SWEEP_LO + (SWEEP_HI - SWEEP_LO) * i / (N - 1);
    int dbm = ccRxRssi(f);
    if (dbm > peak) { peak = dbm; peakF = f; }
    if (dbm < mn) mn = dbm;
    if (dbm > mx) mx = dbm;
    sum += dbm;
    lvl[i] = (uint8_t)(dbm + 120);
  }

  const int px2 = tft.width() / N;
  for (int i = 0; i < N; i++) {
    int dbm = (int)lvl[i] - 120;
    int y = dbmToY(dbm);
    tft.fillRect(i * px2, SPEC_TOP, px2, y - SPEC_TOP, TFT_BLACK);
    tft.fillRect(i * px2, y, px2, SPEC_BOT - y, TFT_CYAN);
  }
  int pi = (int)((peakF - SWEEP_LO) / (SWEEP_HI - SWEEP_LO) * (N - 1));
  tft.fillRect(pi * px2, SPEC_TOP, px2, SPEC_BOT - SPEC_TOP, 0x7BEF);

  char buf[48];
  snprintf(buf, sizeof(buf), "peak %d dBm @ %.1f MHz", peak, peakF);
  tft.fillRect(0, tft.height() - 32, tft.width(), 12, TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(buf, 4, tft.height() - 31, 1);
  Serial.printf("sweep: peak=%d dBm @ %.1f MHz min=%d max=%d avg=%ld", peak, peakF, mn, mx, (long)(sum / N));
  Serial.println();
}

static void draw24Frame() {
  tft.fillRect(0, SB_H, tft.width(), tft.height() - SB_H, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("2.4 GHz SCAN  126 ch", 6, SB_H + 3, 1);
  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("3x NRF24", tft.width() - 6, SB_H + 3, 1);
  tft.setTextDatum(TL_DATUM);
  footer("LEFT/RIGHT view");
}

static void scan24() {
  const int W = tft.width();
  const int top = SB_H + 20, bot = tft.height() - 40;
  static uint8_t busy[126];
  int hits = 0, topCh = -1;

  for (int r = 0; r < 3; r++) {
    int from = r * 42, to = min(126, from + 42);
    for (int ch = from; ch < to; ch++) {
      nrfReg(NRF_CS[r], 0x05, true, (uint8_t)ch);
      digitalWrite(NRF_CE[r], HIGH);
      delayMicroseconds(600);
      bool on = nrfRpd(r);
      if (!on) { delayMicroseconds(200); on = nrfRpd(r); }
      digitalWrite(NRF_CE[r], LOW);
      busy[ch] = on ? 1 : 0;
      if (on) { hits++; if (topCh < 0) topCh = ch; }
    }
  }

  for (int ch = 0; ch < 126; ch++) {
    int x = ch * W / 126, x2 = (ch + 1) * W / 126;
    int h = busy[ch] ? (bot - top) : (bot - top) / 8;
    tft.fillRect(x, bot - h, max(1, x2 - x - 1), h, busy[ch] ? TFT_CYAN : 0x10A2);
  }
  char buf[56];
  snprintf(buf, sizeof(buf), "busy %d/126  top ch %d = %d MHz", hits, topCh,
           2400 + (topCh < 0 ? 0 : topCh));
  tft.fillRect(0, tft.height() - 32, W, 12, TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(buf, 4, tft.height() - 31, 1);
  Serial.printf("scan24: busy=%d/126 top_ch=%d\n", hits, topCh);
  Serial.println();
}

static void drawPantest() {
  Serial.println("selftest: panorama");
  drawStatusBar("SUB-GHz", "CC1101");
  drawPanoramaFrame();
  Serial.println("selftest: sweeping");
  sweepSubGHz();
  Serial.println("selftest: scan24");
  delay(200);
  drawStatusBar("2.4 GHz", "NRF24");
  draw24Frame();
  scan24();
  Serial.println("selftest: back to desktop");
  delay(200);
  drawStatusBar("RF MON", "BAT");
  drawDesktop();
}





void setup() {
  Serial.begin(115200);
  delay(1200);
  Serial.println();
  Serial.println("=== rfmon v2 ===");

  pinMode(BL_PIN, OUTPUT);
  digitalWrite(BL_PIN, HIGH);
  tft.init();
  tft.setRotation(3);
  tft.fillScreen(TFT_BLACK);
  Serial.printf("TFT %dx%d rot3\n", tft.width(), tft.height());

  Wire.begin(8, 9, 100000);
  readBattery();
  Serial.printf("battery %d%% chg=%d\n", g_pct, (int)g_chg);

  for (int i = 0; i < 3; i++) { pinMode(NRF_CS[i], OUTPUT); digitalWrite(NRF_CS[i], HIGH); }
  pinMode(CC_CS, OUTPUT); digitalWrite(CC_CS, HIGH);

  SPI.begin(RF_SCK, RF_MISO, RF_MOSI, -1);
  bool ccOk = ccInit();
  Serial.printf("CC1101 %s", ccOk ? "OK" : "FAIL");
  Serial.println();
  for (int i = 0; i < 3; i++) nrfInit(i);
  Serial.printf("nRF config: %02X %02X %02X\n", nrfRead(NRF_CS[0], 0x00),
                nrfRead(NRF_CS[1], 0x00), nrfRead(NRF_CS[2], 0x00));
  Serial.printf("buttons raw=%02X\n", buttons());

  // самотест: один проход по обоим экранам, чтобы в логе были живые цифры
  drawPantest();
  Serial.println("=== ready ===");
}

void loop() {
  static uint32_t lastBtn = 0, lastBat = 0, lastDraw = 0;
  uint32_t now = millis();

  if (now - lastBat > 2000) {
    lastBat = now;
    int p = g_pct; bool c = g_chg;
    readBattery();
    if (p != g_pct || c != g_chg) {
      drawStatusBar(g_view == 1 ? "SUB-GHz" : g_view == 2 ? "2.4 GHz" : "RF MON",
                    g_view == 1 ? "CC1101" : g_view == 2 ? "NRF24" : "BAT");
      Serial.printf("battery %d%% chg=%d\n", g_pct, (int)g_chg);
    }
  }

  if (now - lastBtn > 150) {
    lastBtn = now;
    static uint8_t lastB = 0xFF;
    uint8_t b = buttons();
    if (b != lastB && b != 0xFF) { Serial.print("btn raw="); Serial.println(b, HEX); }
    lastB = b;
    if (b != 0xFF && b != 0x00) {
      bool up = !(b & (1 << 7)), down = !(b & (1 << 5));
      bool left = !(b & (1 << 3)), right = !(b & (1 << 4)), sel = !(b & (1 << 6));
      if (up && g_view == 0)   { g_sel = (g_sel + N_MOD - 1) % N_MOD; drawDesktop(); }
      if (down && g_view == 0) { g_sel = (g_sel + 1) % N_MOD; drawDesktop(); }
      if (right) { g_view = (g_view + 1) % 3; g_needFrame = true; Serial.printf("view -> %d\n", g_view); }
      if (left)  { g_view = (g_view + 2) % 3; g_needFrame = true; Serial.printf("view -> %d\n", g_view); }
      if (sel) {
        if (g_view == 0) { g_view = (g_sel == 1) ? 2 : 1; }
        else { g_view = 0; }
        g_needFrame = true;
        Serial.printf("sel -> view %d (mod %d)\n", g_view, g_sel);
      }
    }
  }

  if (g_view == 1 || g_view == 2) {
    if (g_needFrame) {
      g_needFrame = false;
      if (g_view == 1) { drawStatusBar("SUB-GHz", "CC1101"); drawPanoramaFrame(); }
      else             { drawStatusBar("2.4 GHz", "NRF24");  draw24Frame(); }
      lastDraw = 0;
    }
    if (now - lastDraw > 40) {
      lastDraw = now;
      if (g_view == 1) sweepSubGHz(); else scan24();
    }
  } else if (g_needFrame) {
    g_needFrame = false;
    drawStatusBar("RF MON", "BAT");
    drawDesktop();
  }
}
