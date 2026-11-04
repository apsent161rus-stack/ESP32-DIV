// div_ident v2 — опрос периферии ESP32-DIV v2 без внешних библиотек.
// Отчёт строится один раз в setup() и перепечатывается в loop().
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <stdarg.h>

static const int PIN_SCK = 12, PIN_MISO = 13, PIN_MOSI = 11;
static const int NRF_CS[3] = {4, 48, 21};
static const int NRF_CE[3] = {15, 47, 14};
static const char* NRF_NAME[3] = {"NRF1", "NRF2", "NRF3"};
static const int CC_CS = 5, CC_GDO0 = 6, CC_GDO2 = 3;
static const int IR_RX = 21;
static const int I2C_SDA = 8, I2C_SCL = 9;

static String REPORT;

static void rline(const char* fmt, ...) {
  char buf[300];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print(buf);
  REPORT += buf;
}

static void nrfCsnLow(uint8_t csn) {
  digitalWrite(CC_CS, HIGH);
  for (int i = 0; i < 3; i++) digitalWrite(NRF_CS[i], HIGH);
  digitalWrite(csn, LOW);
  delayMicroseconds(5);
}

static uint8_t nrfReg(uint8_t csn, uint8_t reg, bool write, uint8_t val) {
  nrfCsnLow(csn);
  SPI.transfer((write ? 0x20 : 0x00) | (reg & 0x1F));
  uint8_t out = 0xFF;
  if (write) SPI.transfer(val);
  else out = SPI.transfer(0xFF);
  digitalWrite(csn, HIGH);
  delayMicroseconds(5);
  return out;
}

static void probeNrf(int idx) {
  const int csn = NRF_CS[idx];
  pinMode(NRF_CE[idx], OUTPUT);
  digitalWrite(NRF_CE[idx], LOW);
  pinMode(csn, OUTPUT);
  digitalWrite(csn, HIGH);
  delay(5);

  uint8_t cfg0 = nrfReg(csn, 0x00, false, 0);
  nrfReg(csn, 0x00, true, 0x0F);
  uint8_t cfgBack = nrfReg(csn, 0x00, false, 0);
  nrfReg(csn, 0x05, true, 0x4C);
  uint8_t chBack = nrfReg(csn, 0x05, false, 0);
  uint8_t setup = nrfReg(csn, 0x06, false, 0);
  uint8_t status = nrfReg(csn, 0x07, false, 0);
  uint8_t fifo = nrfReg(csn, 0x17, false, 0);
  nrfReg(csn, 0x00, true, 0x0C);

  bool ok = (cfgBack == 0x0F) && (chBack == 0x4C);
  rline("[%s] csn=%02d ce=%02d -> %s  cfg0=%02X cfg_w/r=%02X ch_w/r=%02X rf_setup=%02X status=%02X fifo=%02X\n",
        NRF_NAME[idx], csn, NRF_CE[idx], ok ? "ONLINE" : "НЕТ ОТВЕТА",
        cfg0, cfgBack, chBack, setup, status, fifo);
}

static uint8_t ccReadB(uint8_t addr, bool burst) {
  digitalWrite(CC_CS, LOW);
  delayMicroseconds(2);
  SPI.transfer((burst ? 0xC0 : 0x80) | (addr & 0x3F));   // -> status byte
  uint8_t v = SPI.transfer(0x00);        // -> данные регистра
  digitalWrite(CC_CS, HIGH);
  delayMicroseconds(2);
  return v;
}

static uint8_t ccRead(uint8_t addr) { return ccReadB(addr, false); }

static void ccWrite(uint8_t addr, uint8_t val) {
  digitalWrite(CC_CS, LOW);
  delayMicroseconds(2);
  SPI.transfer(addr & 0x3F);
  SPI.transfer(val);
  digitalWrite(CC_CS, HIGH);
  delayMicroseconds(2);
}

static void probeCC() {
  pinMode(CC_CS, OUTPUT);
  digitalWrite(CC_CS, HIGH);
  pinMode(CC_GDO0, INPUT);
  pinMode(CC_GDO2, INPUT);
  delay(5);

  uint8_t part = ccReadB(0x30, true);   // PARTNUM  (статусные регистры — только с burst)
  uint8_t ver  = ccReadB(0x31, true);   // VERSION
  ccWrite(0x0B, 0x06);                  // FSCTRL1
  uint8_t fsBack = ccRead(0x0B);
  ccWrite(0x0B, 0x00);
  uint8_t marc = ccReadB(0x35, true);   // MARCSTATE

  bool ok = (ver == 0x14);
  rline("[CC1101] cs=%02d -> %s  VERSION=%02X (ждём 14) PARTNUM=%02X fsctrl1_w/r=%02X marc=%02X gdo0=%d gdo2=%d\n",
        CC_CS, ok ? "ONLINE" : "НЕТ ОТВЕТА", ver, part, fsBack, marc,
        digitalRead(CC_GDO0), digitalRead(CC_GDO2));
}

static void i2cScan() {
  Wire.begin(I2C_SDA, I2C_SCL, 100000);
  rline("[I2C] sda=%d scl=%d: ", I2C_SDA, I2C_SCL);
  int found = 0;
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      const char* what = "";
      switch (a) {
        case 0x20: case 0x21: what = " PCF8574(кнопки)"; break;
        case 0x24: what = " PN532(NFC)"; break;
        case 0x3C: case 0x3D: what = " OLED"; break;
        case 0x68: what = " MPU/DS3231"; break;
        case 0x75: what = " IP5306(ИБП)"; break;
        case 0x76: case 0x77: what = " BME280"; break;
        default: break;
      }
      rline("%02X%s  ", a, what);
      found++;
      if (a == 0x75) {
        Wire.beginTransmission(0x75);
        Wire.write(0x78);
        Wire.endTransmission(false);
        if (Wire.requestFrom(0x75, 1) == 1) {
          uint8_t pct = Wire.read();
          int p = (pct >> 4) == 0xE ? 25 : (pct >> 4) == 0xC ? 50 : (pct >> 4) == 0x8 ? 75 : 100;
          rline("(0x78=%02X ~%d%%) ", pct, p);
        }
      }
    }
  }
  if (!found) rline("пусто");
  rline("\n");
}

static void probeIR() {
  pinMode(IR_RX, INPUT_PULLUP);
  int idle = digitalRead(IR_RX);
  rline("[IR] rx=%d idle=%d (1 = приёмник жив, линия подтянута)\n", IR_RX, idle);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== div_ident ===");
  Serial.printf("chip=%s cores=%d flash=%luMB heap=%u\n", ESP.getChipModel(), ESP.getChipCores(),
                (unsigned long)(ESP.getFlashChipSize() / (1024 * 1024)), (unsigned)ESP.getFreeHeap());

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  pinMode(CC_CS, OUTPUT); digitalWrite(CC_CS, HIGH);
  for (int i = 0; i < 3; i++) { pinMode(NRF_CS[i], OUTPUT); digitalWrite(NRF_CS[i], HIGH); }

  REPORT = "";
  rline("--- SPI ---\n");
  for (int i = 0; i < 3; i++) probeNrf(i);
  probeCC();
  rline("--- I2C ---\n");
  i2cScan();
  rline("--- прочее ---\n");
  probeIR();
  rline("=== конец ===\n");
  Serial.print(REPORT);
}

void loop() {
  delay(4000);
  Serial.println();
  Serial.println("---- повтор ----");
  Serial.print(REPORT);
}
