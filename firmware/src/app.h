#pragma once
#include <stdint.h>
#include <stddef.h>
#include <LovyanGFX.hpp>
#include "gfx.h"

#ifndef AT_FW_VERSION
#define AT_FW_VERSION "dev"
#endif
#define AT_PROTO 1

enum State : uint8_t { ST_WAITING = 0, ST_DONE = 1, ST_WORKING = 2, ST_IDLE = 3 };

struct Settings {
  uint8_t magic = 0xA7;
  uint8_t version = 3;
  uint8_t brightness = 200;  // 10..255
  bool autodim = true;       // dim after a few quiet minutes
  bool invert = false;       // XOR on top of the panel default
  bool flip = false;         // rotate 180 degrees
  bool swaprb = false;       // swap red/blue in the UI palette (BGR panels)
  bool led = true;           // RGB status LED
  bool sound = false;        // beep on the speaker connector
  bool flash = true;         // pulse a coloured frame round the screen on alerts
  uint8_t gamma = 0;         // 0 = panel default, 1..4 = gamma curve 1/2/3/4 (GAMSET 0x26)
  bool touchCal = false;     // calibration below is valid
  uint16_t cal[8] = {0};
  // appended later: older saved settings are shorter and keep these defaults
  bool sleep = true;         // screen and LED off during the night hours below
  uint8_t sleepFrom = 22;    // local hour the night starts
  uint8_t sleepTo = 9;       // local hour it ends
};

// Everything platform specific lives behind this interface (ESP32 or SDL sim).
struct Hal {
  virtual ~Hal() {}
  virtual uint32_t ms() = 0;
  virtual size_t read(uint8_t* buf, size_t max) = 0;        // host link, non-blocking
  virtual void write(const char* s, size_t n) = 0;
  virtual void led(uint8_t r, uint8_t g, uint8_t b) = 0;    // 0..255 each
  virtual void backlight(uint8_t v) = 0;
  virtual void tone(uint16_t hz, uint16_t ms) {}
  virtual void load(Settings& s) = 0;
  virtual void save(const Settings& s) = 0;
  virtual void applyDisplay(const Settings& s) = 0;         // invert / rotation
  virtual void calibrateTouch(Settings& s) {}
  virtual void setPanel(int which) {}                       // 0 auto, 1 ILI9341, 2 ST7789 (restarts)
  virtual const char* board() = 0;
  virtual const char* panel() = 0;
};

struct Session {
  bool used = false;
  char id[41];
  char src[24];
  char name[88];
  char det[200];
  char cwd[200];
  char pr[408];
  char rp[608];
  State st = ST_IDLE;
  uint32_t sinceMs = 0;  // device clock when the state started
  uint32_t actMs = 0;    // device clock of the last activity (tool call, prompt...)
  uint32_t orderMs = 0;  // actMs as of the last re-sort: keeps the list calm
  uint8_t sub = 0;
  bool ack = true;
  bool focusable = false;  // the bridge can bring its terminal tab to the front
};

enum Screen : uint8_t { SCR_LIST, SCR_DETAIL, SCR_SETTINGS, SCR_TEST, SCR_LEDTEST };

class App {
 public:
  App(lgfx::LGFX_Device& tft, Hal& hal) : tft_(tft), hal_(hal) {}
  void begin();
  void tick();

  // exposed for the simulator's scripted screenshots
  void tapAt(int x, int y);
  void renderNow() { dirty_ = true; render(); }
  Settings& settings() { return cfg_; }

 private:
  static const int MAX_SESSIONS = 24;
  static const int kLineMax = 2048;

  lgfx::LGFX_Device& tft_;
  Hal& hal_;
  Settings cfg_;
  lgfx::LGFX_Sprite band_[2];  // double buffered: draw one while the other is sent by DMA
  Painter p_;
  int W_ = 240, H_ = 320, bandH_ = 40;
  float k_ = 1.0f;  // layout scale relative to a 240 px wide screen

  // fonts (picked by width)
  const AtFont *fSmall_, *fBody_, *fBold_, *fTitle_, *fHuge_;

  // palette
  uint16_t cBg_, cCard_, cCardHi_, cText_, cMuted_, cDim_, cLine_, cAccent_;
  uint16_t cWork_, cWait_, cDone_, cIdle_;

  Session sess_[MAX_SESSIONS];
  int order_[MAX_SESSIONS];
  int count_ = 0;
  uint32_t lastResort_ = 0;

  // host link
  char line_[kLineMax];
  int lineLen_ = 0;
  bool overflow_ = false;
  uint32_t lastRx_ = 0, lastHello_ = 0;
  bool linked_ = false;
  char host_[32] = "";
  int64_t epochAtSync_ = 0;  // host unix time (local tz applied) at syncMs_
  uint32_t syncMs_ = 0;
  int32_t tz_ = 0;

  // ui state
  Screen screen_ = SCR_LIST;
  char detailId_[41] = "";
  int scroll_ = 0, scrollMax_ = 0;
  bool touching_ = false, dragged_ = false, wakeTouch_ = false;
  int touchX0_ = 0, touchY0_ = 0, touchY_ = 0, scroll0_ = 0;
  uint32_t touchT0_ = 0;
  bool dirty_ = true;
  uint32_t lastFrame_ = 0, lastActivity_ = 0;
  bool dimmed_ = false;
  bool sleeping_ = false;       // night hours: backlight and LED off
  uint32_t peekUntil_ = 0;      // a touch at night shows the screen for a while
  uint8_t curBl_ = 0;
  int lastWaiting_ = 0, lastDone_ = 0;
  uint32_t flashUntil_ = 0;

  // LED test: the host drives the LED directly (colour + pattern) to try how
  // colours read through the light pipe. Ends on its own after led_ms.
  // Alert frame: a few pulses of colour round the screen edge when a session
  // starts waiting for you (red) or finishes (green).
  // The frame, the LED and every "needs you" pulse on screen share one beat that
  // restarts with each alert, so they all rise and fall together.
  uint16_t frameColor_ = 0;
  uint8_t alertRgb_[3] = {0, 0, 0};
  uint32_t beatStart_ = 0, alertUntil_ = 0;
  bool frameOn_ = false;
  char alertId_[41] = "";

  // Focus button on the detail screen: 0 idle, 1 waiting for the bridge, 2 ok, 3 failed
  uint8_t focusState_ = 0;
  uint32_t focusAt_ = 0;
  char focusMsg_[28] = "";
  int focusBtnX_ = 0;  // the session that raised the alert: its card pulses too

  uint8_t ledTest_[3] = {0, 0, 0};
  uint8_t ledTestPat_ = 0;
  uint32_t ledTestUntil_ = 0;
  char ledTestLabel_[40] = "";

  void setupPalette();
  void pollLink();
  void handleLine(char* s);
  void sendHello();
  void sendCfg();
  void send(const char* json);
  Session* find(const char* id);
  Session* alloc(const char* id);
  void sortSessions();
  void counts(int& waiting, int& done, int& working, int& idle);
  void onStateChange(int waiting, int done);
  void updateLed();
  void updateBacklight();
  bool nightNow() const;
  void pollTouch();
  void onTap(int x, int y);
  void onLongPress(int x, int y);
  void ackSession(Session* s);

  void render();
  void pushBand(lgfx::LGFX_Sprite& band, int y);
  void drawScreen();
  void drawHeader(int waiting, int done, int working);
  void drawFooter();
  void drawList();
  void drawCard(const Session& s, int y, int h);
  void drawEmpty();
  void drawDetail();
  void drawSettings();
  void drawTest();
  void drawLedTest();
  void drawFrame();
  void startAlert(uint16_t color, uint8_t r, uint8_t g, uint8_t b);
  float beat() const;
  float ledPattern(int pat) const;
  void drawStatusIcon(State st, int cx, int cy, int r);
  void drawToggle(int x, int y, bool on);
  void drawBack(const char* title);

  uint16_t stateColor(State st) const;
  const char* stateLabel(State st) const;
  void ageText(uint32_t sinceMs, char* out, size_t n) const;
  int S(int v) const { return (int)(v * k_ + 0.5f); }
  float pulse(uint32_t periodMs) const;
  bool animating() const;
  int listTop() const { return S(40); }
  int listBottom() const { return H_ - S(22); }
  int cardH() const { return S(58); }
  int cardGap() const { return S(6); }
};
