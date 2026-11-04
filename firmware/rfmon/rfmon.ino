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
SPIClass radioSPI(HSPI);          // SPI3 — отдельно от TFT (SPI2)

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
  if (Wire.endTransmission(false) != 0) return 0xFF;
  if (Wire.requestFrom(PCF8574, 1) != 1) return 0xFF;
  return Wire.read();
}

/* ---------------- CC1101 ---------------- */
static void ccWrite(uint8_t addr, uint8_t val) {
  digitalWrite(CC_CS, LOW);
  radioSPI.transfer(addr & 0x3F);
  radioSPI.transfer(val);
  digitalWrite(CC_CS, HIGH);
}
static uint8_t ccReadB(uint8_t addr, bool burst) {
  digitalWrite(CC_CS, LOW);
  radioSPI.transfer((burst ? 0xC0 : 0x80) | (addr & 0x3F));
  uint8_t v = radioSPI.transfer(0x00);
  digitalWrite(CC_CS, HIGH);
  return v;
}
static void ccStrobe(uint8_t cmd) {
  digitalWrite(CC_CS, LOW);
  radioSPI.transfer(cmd);
  digitalWrite(CC_CS, HIGH);
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

static void ccConfig() {
  const uint8_t regs[][2] = {
    {0x00,0x0B},{0x02,0x0D},{0x03,0x07},{0x04,0xD3},{0x05,0x91},{0x06,0xFF},
    {0x07,0x04},{0x08,0x32},{0x0A,0x00},{0x0B,0x06},{0x0C,0x00},
    {0x15,0xF5},{0x16,0x83},{0x17,0x13},{0x18,0x22},{0x19,0xF8},{0x1A,0x15},
    {0x1B,0x07},{0x1C,0x30},{0x1D,0x18},{0x1E,0x16},{0x1F,0x6C},
    {0x20,0x03},{0x21,0x40},{0x22,0x91},{0x23,0xFB},{0x24,0x56},{0x25,0x10},
    {0x26,0xE9},{0x27,0x2A},{0x28,0x00},{0x29,0x1F},{0x2C,0x81},{0x2D,0x35},{0x2E,0x09},
  };
  for (auto& r : regs) ccWrite(r[0], r[1]);
  ccWrite(0x3E, 0x8E);
  ccSetFreq(433.92f);
  ccStrobe(0x36);
}

/* ---------------- nRF24 ---------------- */
static void nrfReg(uint8_t csn, uint8_t reg, bool write, uint8_t val) {
  for (int i = 0; i < 3; i++) digitalWrite(NRF_CS[i], HIGH);
  digitalWrite(CC_CS, HIGH);
  digitalWrite(csn, LOW);
  radioSPI.transfer((write ? 0x20 : 0x00) | (reg & 0x1F));
  if (write) radioSPI.transfer(val); else radioSPI.transfer(0xFF);
  digitalWrite(csn, HIGH);
}
static uint8_t nrfRead(uint8_t csn, uint8_t reg) {
  for (int i = 0; i < 3; i++) digitalWrite(NRF_CS[i], HIGH);
  digitalWrite(CC_CS, HIGH);
  digitalWrite(csn, LOW);
  radioSPI.transfer(reg & 0x1F);
  uint8_t v = radioSPI.transfer(0xFF);
  digitalWrite(csn, HIGH);
  return v;
}
static void nrfInit(int i) {
  pinMode(NRF_CE[i], OUTPUT);
  digitalWrite(NRF_CE[i], LOW);
  pinMode(NRF_CS[i], OUTPUT);
  digitalWrite(NRF_CS[i], HIGH);
  nrfReg(NRF_CS[i], 0x01, true, 0x00);
  nrfReg(NRF_CS[i], 0x02, true, 0x00);
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

static void drawDesktop() {
  tft.fillRect(0, SB_H, tft.width(), tft.height() - SB_H, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("MODULES", 6, SB_H + 4, 2);
  for (int i = 0; i < N_MOD; i++) {
    int y = SB_H + 26 + i * 26;
    bool sel = (i == g_sel);
    if (sel) tft.fillRect(4, y - 3, tft.width() - 8, 22, 0x0A52);
    tft.drawRect(4, y - 3, tft.width() - 8, 22, sel ? TFT_CYAN : 0x18E3);
    tft.setTextColor(sel ? TFT_WHITE : TFT_LIGHTGREY, sel ? 0x0A52 : TFT_BLACK);
    tft.drawString(MODULES[i], 10, y, 1);
  }
  footer("UP/DOWN  OK=open  MENU");
}

#define SPEC_TOP  (SB_H + 16)
#define SPEC_BOT  (tft.height() - 34)
static const float SWEEP_LO = 281.0f, SWEEP_HI = 928.0f;

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
  tft.drawString("281-928 MHz CC1101", tft.width() - 6, SB_H + 3, 1);
  tft.setTextDatum(TL_DATUM);
  for (int i = 1; i < 4; i++) {
    int y = SPEC_TOP + i * (SPEC_BOT - SPEC_TOP) / 4;
    tft.drawFastHLine(0, y, tft.width(), 0x10A2);
  }
  footer("LEFT/RIGHT view");
}

static void sweepSubGHz() {
  const int W = tft.width();
  int peak = -127; float peakF = 0;
  long sum = 0; int mn = 127, mx = -127;
  for (int i = 0; i < W; i++) {
    float f = SWEEP_LO + (SWEEP_HI - SWEEP_LO) * i / (W - 1);
    ccSetFreq(f);
    ccStrobe(0x34);
    delayMicroseconds(420);
    int dbm = ccRssiDbm();
    ccStrobe(0x36);
    if (dbm > peak) { peak = dbm; peakF = f; }
    if (dbm < mn) mn = dbm;
    if (dbm > mx) mx = dbm;
    sum += dbm;
    if ((i % 80) == 0) Serial.printf("  sweep col %d f=%.1f dbm=%d\n", i, f, dbm);

    int y = dbmToY(dbm);
    tft.drawFastVLine(i, SPEC_TOP, y - SPEC_TOP, TFT_BLACK);
    tft.drawFastVLine(i, y, SPEC_BOT - y, TFT_CYAN);
  }
  int px = (int)((peakF - SWEEP_LO) / (SWEEP_HI - SWEEP_LO) * (W - 1));
  tft.drawFastVLine(px, SPEC_TOP, SPEC_BOT - SPEC_TOP, 0x7BEF);
  char buf[48];
  snprintf(buf, sizeof(buf), "peak %d dBm @ %.1f MHz", peak, peakF);
  tft.fillRect(0, tft.height() - 32, W, 12, TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(buf, 4, tft.height() - 31, 1);
  Serial.printf("sweep: peak=%d dBm @ %.1f MHz min=%d max=%d avg=%ld marc=%02X\n",
                peak, peakF, mn, mx, (long)(sum / W), ccReadB(0x35, true));
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
  uint8_t busy[126];
  int hits = 0, topCh = -1;
  const int chunk = 42;
  for (int r = 0; r < 3; r++) {
    int from = r * chunk, to = min(126, from + chunk);
    for (int ch = from; ch < to; ch++) {
      nrfReg(NRF_CS[r], 0x05, true, (uint8_t)ch);
      digitalWrite(NRF_CE[r], HIGH);
      delayMicroseconds(420);
      bool on = nrfRpd(r);
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

  radioSPI.begin(RF_SCK, RF_MISO, RF_MOSI, -1);
  radioSPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  for (int i = 0; i < 3; i++) { pinMode(NRF_CS[i], OUTPUT); digitalWrite(NRF_CS[i], HIGH); }
  pinMode(CC_CS, OUTPUT); digitalWrite(CC_CS, HIGH);

  uint8_t v0 = ccReadB(0x31, true);
  ccReset();
  uint8_t v1 = ccReadB(0x31, true);
  ccConfig();
  uint8_t v2 = ccReadB(0x31, true);
  delay(50);
  uint8_t v3 = ccReadB(0x31, true);
  uint8_t ver = v3;
  Serial.printf("CC1101 v0=%02X afterReset=%02X afterCfg=%02X now=%02X v3=%02X %s\n", v0, v1, v2, ver, v3, ver == 0x14 ? "OK" : "FAIL");
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
    uint8_t b = buttons();
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
