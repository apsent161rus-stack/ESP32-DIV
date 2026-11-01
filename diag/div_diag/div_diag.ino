/*
 * ESP32-DIV v2 — hardware diagnostics
 * Проверяет, живы ли модули и на какой мощности они реально работают.
 * Результат выводится на экран крупно: фото экрана = отчёт.
 */

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <TFT_eSPI.h>
#include <RF24.h>
#include <ELECHOUSE_CC1101_SRC_DRV.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRutils.h>

/* ---- pins (v2 board) ---- */
#define BACKLIGHT_PIN 7
#define RF_SCK   12
#define RF_MISO  13
#define RF_MOSI  11

#define NRF1_CE  15
#define NRF1_CSN 4
#define NRF2_CE  47
#define NRF2_CSN 48
#define NRF3_CE  14
#define NRF3_CSN 21

#define CC_CS    5
#define CC_GDO0  6
#define CC_GDO2  3

#define IR_RX_PIN 21

TFT_eSPI tft = TFT_eSPI();

RF24 nrf1(NRF1_CE, NRF1_CSN);
RF24 nrf2(NRF2_CE, NRF2_CSN);
RF24 nrf3(NRF3_CE, NRF3_CSN);

IRrecv irrecv(IR_RX_PIN);
decode_results irResults;

int      g_line = 0;

void line(const char* text, uint16_t color) {
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(text, 6, 24 + g_line * 18, 2);
  g_line++;
}

void header(const char* title) {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, tft.width(), 20, 0x05FF);
  tft.setTextColor(TFT_BLACK, 0x05FF);
  tft.drawString(title, 6, 2, 2);
  g_line = 0;
}

const char* paName(uint8_t pa) {
  switch (pa) {
    case RF24_PA_MIN:  return "-18dBm";
    case RF24_PA_LOW:  return "-12dBm";
    case RF24_PA_HIGH: return "-6dBm";
    case RF24_PA_MAX:  return "0dBm";
    default:           return "?";
  }
}

void testNrf(const char* name, RF24& radio) {
  bool ok = radio.begin();
  char buf[64];

  if (!ok) {
    snprintf(buf, sizeof(buf), "%s: begin FAIL", name);
    line(buf, TFT_RED);
    return;
  }

  const bool connected = radio.isChipConnected();
  radio.setChannel(76);
  radio.setDataRate(RF24_1MBPS);
  radio.setPALevel(RF24_PA_MAX);
  radio.setAutoAck(false);
  radio.startListening();

  const uint8_t pa   = radio.getPALevel();
  const uint8_t rate = radio.getDataRate();

  snprintf(buf, sizeof(buf), "%s %s PA=%s R=%d",
           name, connected ? "OK" : "NOCHIP", paName(pa), rate);
  line(buf, connected ? TFT_GREEN : TFT_ORANGE);
}

void setup() {
  pinMode(BACKLIGHT_PIN, OUTPUT);
  digitalWrite(BACKLIGHT_PIN, HIGH);

  Serial.begin(115200);
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  Wire.begin();

  header("MODULE TEST");

  /* ---- NRF24 x3 ---- */
  SPI.begin(RF_SCK, RF_MISO, RF_MOSI);

  testNrf("NRF1", nrf1);
  testNrf("NRF2", nrf2);
  testNrf("NRF3", nrf3);

  nrf1.powerDown();
  nrf2.powerDown();
  nrf3.powerDown();

  /* ---- CC1101 ---- */
  ELECHOUSE_cc1101.setSpiPin(RF_SCK, RF_MISO, RF_MOSI, CC_CS);
  ELECHOUSE_cc1101.setGDO(CC_GDO0, CC_GDO2);
  ELECHOUSE_cc1101.Init();

  const uint8_t ccVersion = ELECHOUSE_cc1101.getCC1101();
  char buf[64];
  if (ccVersion == 0x14) {
    ELECHOUSE_cc1101.setMHZ(433.92);
    ELECHOUSE_cc1101.setPA(12);          // максимальный уровень PA-таблицы (~+10 dBm)
    ELECHOUSE_cc1101.SetTx();
    const uint8_t paReg = ELECHOUSE_cc1101.SpiReadReg(CC1101_PATABLE);
    snprintf(buf, sizeof(buf), "CC1101 OK ver=14 PATBL=%02X", paReg);
    line(buf, TFT_GREEN);
  } else {
    snprintf(buf, sizeof(buf), "CC1101 NO ver=%02X", ccVersion);
    line(buf, TFT_RED);
  }

  /* ---- IR: ждём код с пульта 10 секунд ---- */
  irrecv.enableIRIn();
  line("IR: press remote...", TFT_YELLOW);

  const uint32_t start = millis();
  bool gotIr = false;
  while (millis() - start < 10000) {
    if (irrecv.decode(&irResults)) {
      snprintf(buf, sizeof(buf), "IR OK %s", uint64ToString(irResults.value, 16).c_str());
      line(buf, TFT_GREEN);
      irrecv.resume();
      gotIr = true;
      break;
    }
    delay(50);
  }
  if (!gotIr) {
    line("IR: no signal (off?)", TFT_ORANGE);
  }

  line("done - photo this", TFT_WHITE);
}

void loop() {
  delay(1000);
}
