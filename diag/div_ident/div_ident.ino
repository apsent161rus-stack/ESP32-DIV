// div_ident — опрос периферии ESP32-DIV v2 без внешних библиотек.
// Печатает в USB-CDC (COM4): что реально отвечает на SPI/I2C.
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>

static const int PIN_SCK = 12, PIN_MISO = 13, PIN_MOSI = 11;
static const int NRF_CS[3] = {4, 48, 21};
static const int NRF_CE[3] = {15, 47, 14};
static const char* NRF_NAME[3] = {"NRF1", "NRF2", "NRF3"};
static const int CC_CS = 5, CC_GDO0 = 6, CC_GDO2 = 3;
static const int IR_RX = 21;        // делит пин с NRF3_CSN
static const int I2C_SDA = 8, I2C_SCL = 9;

static void spiStart() {
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
}

static uint8_t nrfReg(uint8_t csn, uint8_t reg, bool write, uint8_t val) {
  digitalWrite(csn, LOW);
  delayMicroseconds(5);
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
  digitalWrite(NRF_CE[idx], LOW);
  pinMode(NRF_CE[idx], OUTPUT);
  digitalWrite(NRF_CE[idx], LOW);
  pinMode(csn, OUTPUT);
  digitalWrite(csn, HIGH);
  delay(10);

  uint8_t cfg0   = nrfReg(csn, 0x00, false, 0);
  nrfReg(csn, 0x00, true, 0x0F);              // CONFIG: PWR_UP, CRC on
  uint8_t cfgBack = nrfReg(csn, 0x00, false, 0);
  nrfReg(csn, 0x05, true, 0x4C);              // RF_CH = 76
  uint8_t chBack = nrfReg(csn, 0x05, false, 0);
  uint8_t setup  = nrfReg(csn, 0x06, false, 0);
  uint8_t status = nrfReg(csn, 0x07, false, 0);
  uint8_t fifo   = nrfReg(csn, 0x17, false, 0);
  nrfReg(csn, 0x00, true, 0x0C);              // standby, CRC on

  bool ok = (cfgBack == 0x0F) && (chBack == 0x4C);
  Serial.printf("[%s] csn=%02d ce=%02d -> %s  cfg0=%02X cfg_w/r=%02X  ch_w/r=%02X  rf_setup=%02X  status=%02X fifo=%02X\n",
                NRF_NAME[idx], csn, NRF_CE[idx], ok ? "ONLINE" : "НЕТ ОТВЕТА",
                cfg0, cfgBack, chBack, setup, status, fifo);
}

static uint8_t ccRead(uint8_t addr) {
  digitalWrite(CC_CS, LOW);
  delayMicroseconds(2);
  SPI.transfer(0x80 | (addr & 0x3F));
  SPI.transfer(0x00);                 // первый байт — status
  uint8_t v = SPI.transfer(0x00);      // второй — данные
  digitalWrite(CC_CS, HIGH);
  delayMicroseconds(2);
  return v;
}

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
  delay(10);

  uint8_t ver = ccRead(0x31);         // VERSION -> 0x14 у CC1101
  uint8_t part = ccRead(0x30);        // PARTNUM -> 0x00
  ccWrite(0x0B, 0x06);                // FSCTRL1
  uint8_t fsBack = ccRead(0x0B);
  ccWrite(0x0B, 0x00);

  bool ok = (ver == 0x14);
  Serial.printf("[CC1101] cs=%02d -> %s  VERSION=%02X (ждём 14) PARTNUM=%02X  fsctrl1_w/r=%02X  gdo0=%d gdo2=%d\n",
                CC_CS, ok ? "ONLINE" : "НЕТ ОТВЕТА", ver, part, fsBack,
                digitalRead(CC_GDO0), digitalRead(CC_GDO2));
}

static void i2cScan() {
  Wire.begin(I2C_SDA, I2C_SCL, 100000);
  Serial.printf("[I2C] sda=%d scl=%d: ", I2C_SDA, I2C_SCL);
  int found = 0;
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      const char* what = "";
      switch (a) {
        case 0x20: case 0x21: what = " PCF8574 (кнопки)"; break;
        case 0x24: what = " PN532 (NFC)"; break;
        case 0x3C: case 0x3D: what = " OLED/дисплей"; break;
        case 0x68: what = " MPU/DS3231"; break;
        case 0x75: what = " IP5306 (ИБП/батарея)"; break;
        case 0x76: case 0x77: what = " BME280/BMP"; break;
        default: break;
      }
      Serial.printf("%02X%s  ", a, what);
      found++;
      if (a == 0x75) {
        uint8_t pct = 0;
        Wire.beginTransmission(0x75); Wire.write(0x78); Wire.endTransmission(false);
        if (Wire.requestFrom(0x75, 1) == 1) {
          pct = Wire.read();
          Serial.printf("(IP5306 0x78=%02X -> %d%%) ", pct, (pct >> 4) == 0xE ? 25 : (pct >> 4) == 0xC ? 50 : (pct >> 4) == 0x8 ? 75 : 100);
        }
      }
    }
  }
  if (!found) Serial.print("пусто");
  Serial.println();
}

static void probeIR() {
  pinMode(IR_RX, INPUT_PULLUP);
  int idle = digitalRead(IR_RX);
  Serial.printf("[IR] rx=%d idle=%d (1 = линия подтянута, приёмник жив; 0 = линия посажена)\n", IR_RX, idle);
  pinMode(IR_RX, OUTPUT);
  digitalWrite(IR_RX, HIGH);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== div_ident: опрос периферии ===");
  Serial.printf("chip=%s cores=%d flash=%lu MB  heap=%u\n",
                ESP.getChipModel(), ESP.getChipCores(),
                (unsigned long)(ESP.getFlashChipSize() / (1024 * 1024)), (unsigned)ESP.getFreeHeap());
  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  pinMode(CC_CS, OUTPUT); digitalWrite(CC_CS, HIGH);
  for (int i = 0; i < 3; i++) { pinMode(NRF_CS[i], OUTPUT); digitalWrite(NRF_CS[i], HIGH); }
  spiStart();
  Serial.println("--- SPI ---");
  for (int i = 0; i < 3; i++) probeNrf(i);
  probeCC();
  Serial.println("--- I2C ---");
  i2cScan();
  Serial.println("--- прочее ---");
  probeIR();
  Serial.println("=== конец ===");
}

void loop() {
  // повторяем отчёт, чтобы поздно подключившийся хост его увидел
  delay(4000);
  Serial.println("-- повтор --");
  spiStart();
  for (int i = 0; i < 3; i++) probeNrf(i);
  probeCC();
  i2cScan();
}
