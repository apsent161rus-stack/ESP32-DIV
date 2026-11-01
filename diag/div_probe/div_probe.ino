/*
 * ESP32-DIV v2 — SPI bus probe (v2 diag)
 * Прямое чтение регистров без библиотек RF24/CC1101: перебираем CS-пины и
 * смотрим, кто отзывается. Плюс IR-тест и поворот экрана.
 */

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <TFT_eSPI.h>
#include <IRrecv.h>
#include <IRutils.h>

#define BACKLIGHT_PIN 7
#define SCK  12
#define MISO 13
#define MOSI 11

TFT_eSPI tft = TFT_eSPI();

int g_line = 0;
void line(const char* text, uint16_t color) {
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(text, 6, 26 + g_line * 18, 2);
  g_line++;
}

uint8_t spiRead(uint8_t cs, uint8_t reg) {
  pinMode(cs, OUTPUT);
  digitalWrite(cs, HIGH);
  SPI.beginTransaction(SPISettings(4000000, MSBFIRST, SPI_MODE0));
  digitalWrite(cs, LOW);
  delayMicroseconds(10);
  const uint8_t v = SPI.transfer(reg);
  digitalWrite(cs, HIGH);
  SPI.endTransaction();
  return v;
}

IRrecv irrecv(21);
decode_results irResults;

void setup() {
  pinMode(BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BACKLIGHT_PIN, HIGH);

  tft.init();
  tft.setRotation(2);
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, tft.width(), 22, 0x05FF);
  tft.setTextColor(TFT_BLACK, 0x05FF);
  tft.drawString("SPI BUS PROBE", 6, 3, 2);

  Wire.begin();
  SPI.begin(SCK, MISO, MOSI, -1);

  char buf[64];

  /* --- NRF24: STATUS (0x07) на трёх CSN --- */
  const uint8_t csList[3] = {4, 48, 21};
  for (int i = 0; i < 3; i++) {
    const uint8_t st = spiRead(csList[i], 0x07);
    const uint8_t cfg = spiRead(csList[i], 0x00);
    snprintf(buf, sizeof(buf), "CSN%u STA=%02X CFG=%02X %s",
             csList[i], st, cfg, (cfg != 0x00 && cfg != 0xFF) ? "OK" : "no");
    line(buf, (cfg != 0x00 && cfg != 0xFF) ? TFT_GREEN : TFT_ORANGE);
  }

  /* --- CC1101: VERSION (0x31 | 0x80 = 0xB1), ожидаем 0x14 --- */
  const uint8_t ccCs[3] = {5, 21, 4};
  for (int i = 0; i < 3; i++) {
    const uint8_t ver = spiRead(ccCs[i], 0xB1);
    snprintf(buf, sizeof(buf), "CC cs%u VER=%02X %s", ccCs[i], ver,
             ver == 0x14 ? "OK" : "-");
    line(buf, ver == 0x14 ? TFT_GREEN : TFT_ORANGE);
  }

  /* --- IR --- */
  irrecv.enableIRIn();
  line("IR: press remote 10s", TFT_YELLOW);
  const uint32_t start = millis();
  bool got = false;
  while (millis() - start < 10000) {
    if (irrecv.decode(&irResults)) {
      snprintf(buf, sizeof(buf), "IR OK val=%s", uint64ToString(irResults.value, 16).c_str());
      line(buf, TFT_GREEN);
      irrecv.resume();
      got = true;
      break;
    }
    delay(40);
  }
  if (!got) line("IR: no signal", TFT_ORANGE);
}

void loop() { delay(1000); }
