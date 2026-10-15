// ESP32 entry point (Arduino core).
#include <Arduino.h>
#include <Preferences.h>

#include "boards.h"
#include "../app.h"

static LGFX tft;

static const int PIN_LED_R = 4, PIN_LED_G = 16, PIN_LED_B = 17;  // active low
static const int PIN_SPEAKER = 26;
static const int CH_R = 1, CH_G = 2, CH_B = 3, CH_TONE = 4;

class EspHal : public Hal {
 public:
  uint8_t lr = 1, lg = 1, lb = 1;  // force first write
  bool panelInvert = false;
  const char* panelName = "?";

  uint32_t ms() override { return millis(); }

  size_t read(uint8_t* buf, size_t max) override {
    int n = Serial.available();
    if (n <= 0) return 0;
    if ((size_t)n > max) n = max;
    return Serial.readBytes(buf, n);
  }

  void write(const char* s, size_t n) override { Serial.write((const uint8_t*)s, n); }

  void led(uint8_t r, uint8_t g, uint8_t b) override {
    if (r == lr && g == lg && b == lb) return;
    lr = r; lg = g; lb = b;
    ledcWrite(CH_R, 255 - r);
    ledcWrite(CH_G, 255 - g);
    ledcWrite(CH_B, 255 - b);
  }

  void backlight(uint8_t v) override { tft.setBrightness(v); }

  void tone(uint16_t hz, uint16_t dur) override {
    ledcAttachPin(PIN_SPEAKER, CH_TONE);
    ledcWriteTone(CH_TONE, hz);
    delay(dur);
    ledcWriteTone(CH_TONE, 0);
    ledcDetachPin(PIN_SPEAKER);
    pinMode(PIN_SPEAKER, OUTPUT);
    digitalWrite(PIN_SPEAKER, LOW);
  }

  void load(Settings& s) override {
    Preferences p;
    p.begin("agenttally", true);
    Settings tmp;
    if (p.getBytesLength("cfg") == sizeof(Settings)) {
      p.getBytes("cfg", &tmp, sizeof tmp);
      if (tmp.magic == s.magic && tmp.version == s.version) s = tmp;
    }
    p.end();
    if (s.touchCal) tft.setTouchCalibrate(s.cal);
  }

  void save(const Settings& s) override {
    Preferences p;
    p.begin("agenttally", false);
    p.putBytes("cfg", &s, sizeof s);
    p.end();
  }

  void applyDisplay(const Settings& s) override {
    tft.setRotation(s.flip ? 2 : 0);
    tft.invertDisplay(s.invert);
    // gamma 0 = board default. The ST7789 on dual-USB 2432S028 boards looks blue-ish
    // in the dark tones with the stock curve; curve 2 renders neutral greys.
    int g = s.gamma;
#if defined(AT_PANEL_SELECT)
    if (!g && !tft.isIli) g = 2;
#endif
    if (g >= 1 && g <= 4) {
      // GAMSET: same opcode on ILI9341 and ST7789, selects one of four built-in curves.
      tft.startWrite();
      tft.writeCommand(0x26);
      tft.writeData(1 << (g - 1));
      tft.endWrite();
    }
  }

  void calibrateTouch(Settings& s) override {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.setTextDatum(textdatum_t::middle_center);
    tft.setFont(&fonts::Font2);
    tft.drawString("Touch the arrow tips", tft.width() / 2, tft.height() / 2);
    tft.calibrateTouch(s.cal, TFT_WHITE, TFT_BLACK, 16);
    s.touchCal = true;
    tft.setTouchCalibrate(s.cal);
  }

  void setPanel(int which) override {
    Preferences p;
    p.begin("agenttally", false);
    p.putUChar("panel", (uint8_t)which);
    p.end();
    delay(50);
    ESP.restart();
  }

  const char* board() override { return AT_BOARD_NAME; }
  const char* panel() override { return panelName; }
};

static EspHal hal;
static App app(tft, hal);

void setup() {
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);

  ledcSetup(CH_R, 5000, 8); ledcAttachPin(PIN_LED_R, CH_R);
  ledcSetup(CH_G, 5000, 8); ledcAttachPin(PIN_LED_G, CH_G);
  ledcSetup(CH_B, 5000, 8); ledcAttachPin(PIN_LED_B, CH_B);
  hal.led(0, 0, 0);
  pinMode(PIN_SPEAKER, OUTPUT);
  digitalWrite(PIN_SPEAKER, LOW);

#if defined(AT_PANEL_SELECT)
  {
    Preferences p;
    p.begin("agenttally", true);
    int force = p.getUChar("panel", 0);
    p.end();
    tft.select(force);
    hal.panelName = tft.panelName();
  }
#else
  hal.panelName = AT_PANEL_NAME;
#endif
  tft.init();
  app.begin();
#if defined(AT_PANEL_SELECT)
  char b[120];
  snprintf(b, sizeof b, "{\"t\":\"log\",\"m\":\"panel %s id=%08lx st=%08lx w=%d\"}\n", tft.panelName(),
           (unsigned long)tft.probeId, (unsigned long)tft.probeSt, (int)tft.width());
  Serial.print(b);
#endif
}

void loop() {
  app.tick();
  delay(5);
}
