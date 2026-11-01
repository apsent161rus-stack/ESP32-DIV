/*
 * ESP32-DIV v2 — pin sweep
 * Перебирает CS-кандидаты и показывает только те линии, где отозвался чип:
 *   NRF24 — CONFIG (0x00) = 0x0E/0x0F, STATUS (0x07) != 0x00/0xFF
 *   CC1101 — VERSION (0x31|0x80) = 0x14, PARTNUM (0x30|0x80) = 0x00
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

static char g_report[24][48];
static int  g_reportN = 0;

void line(const char* text, uint16_t color) {
  tft.setTextColor(color, TFT_BLACK);
  tft.drawString(text, 6, 26 + g_line * 18, 2);
  Serial.println(text);
  if (g_reportN < 24) { strncpy(g_report[g_reportN], text, 47); g_report[g_reportN][47] = 0; g_reportN++; }
  g_line++;
}

uint8_t spiRead(uint8_t cs, uint8_t reg) {
  pinMode(cs, OUTPUT);
  digitalWrite(cs, HIGH);
  SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
  digitalWrite(cs, LOW);
  delayMicroseconds(20);
  const uint8_t v = SPI.transfer(reg);
  delayMicroseconds(20);
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
  tft.drawString("PIN SWEEP", 6, 3, 2);

  Serial.begin(115200);
  delay(300);
  Serial.println("=== SWEEP START ===");
  Wire.begin();
  SPI.begin(SCK, MISO, MOSI, -1);

  const uint8_t pins[] = {2, 4, 5, 10, 14, 15, 18, 21, 47, 48};
  char buf[64];

  for (uint8_t cs : pins) {
    const uint8_t cfg = spiRead(cs, 0x00);
    const uint8_t st  = spiRead(cs, 0x07);
    if (cfg == 0x0E || cfg == 0x0F) {
      snprintf(buf, sizeof(buf), "cs%u NRF cfg=%02X st=%02X", cs, cfg, st);
      line(buf, TFT_GREEN);
    }
  }

  for (uint8_t cs : pins) {
    const uint8_t ver = spiRead(cs, 0xB1);
    const uint8_t pn  = spiRead(cs, 0xB0);
    if (ver == 0x14) {
      snprintf(buf, sizeof(buf), "cs%u CC1101 ver=%02X pn=%02X", cs, ver, pn);
      line(buf, TFT_CYAN);
    }
  }

  line("--- IR test 10s ---", TFT_YELLOW);
  irrecv.enableIRIn();
  const uint32_t start = millis();
  bool got = false;
  while (millis() - start < 10000) {
    if (irrecv.decode(&irResults)) {
      snprintf(buf, sizeof(buf), "IR OK %s", uint64ToString(irResults.value, 16).c_str());
      line(buf, TFT_GREEN);
      irrecv.resume();
      got = true;
      break;
    }
    delay(30);
  }
  if (!got) line("IR: nothing", TFT_ORANGE);
}

void loop() {
  // Re-emit the report so the host can read it whenever it attaches.
  Serial.println("=== REPORT ===");
  for (int i = 0; i < g_reportN; i++) Serial.println(g_report[i]);
  Serial.println("=== END ===");
  delay(3000);
}
