#include "app.h"
#include <ArduinoJson.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fonts/inter_b14.h"
#include "fonts/inter_b19.h"
#include "fonts/inter_b28.h"
#include "fonts/inter_r12.h"
#include "fonts/inter_r13.h"
#if !defined(AT_SMALL_ONLY)
#include "fonts/inter_b18.h"
#include "fonts/inter_b25.h"
#include "fonts/inter_b36.h"
#include "fonts/inter_r16.h"
#include "fonts/inter_r17.h"
#endif

// ------------------------------------------------------------------ helpers

// Copy at most n-1 bytes without splitting a UTF-8 sequence.
static void copyUtf8(char* dst, size_t n, const char* src) {
  if (!src) src = "";
  size_t len = strlen(src);
  if (len >= n) {
    len = n - 1;
    while (len > 0 && ((uint8_t)src[len] & 0xC0) == 0x80) len--;
  }
  memcpy(dst, src, len);
  dst[len] = 0;
}

static State parseState(const char* s) {
  if (!s) return ST_IDLE;
  if (!strcmp(s, "working")) return ST_WORKING;
  if (!strcmp(s, "waiting")) return ST_WAITING;
  if (!strcmp(s, "done")) return ST_DONE;
  return ST_IDLE;
}

// Sort groups: 0 = needs you, 1 = active (working, or done and not seen yet),
// 2 = quiet (idle, or done and already seen). Within a group: see sortSessions().
static int rank(const Session& s) {
  if (s.st == ST_WAITING) return 0;
  if (s.st == ST_WORKING || (s.st == ST_DONE && !s.ack)) return 1;
  return 2;
}

// ------------------------------------------------------------------ setup

void App::begin() {
  hal_.load(cfg_);
  hal_.applyDisplay(cfg_);
  W_ = tft_.width();
  H_ = tft_.height();
  k_ = W_ / 240.0f;
#if !defined(AT_SMALL_ONLY)
  if (W_ >= 300) {
    fSmall_ = &inter_r16; fBody_ = &inter_r17; fBold_ = &inter_b18; fTitle_ = &inter_b25; fHuge_ = &inter_b36;
  } else
#endif
  {
    fSmall_ = &inter_r12; fBody_ = &inter_r13; fBold_ = &inter_b14; fTitle_ = &inter_b19; fHuge_ = &inter_b28;
  }
  setupPalette();

  bandH_ = 40;
  for (auto& b : band_) b.setColorDepth(16);
  while (bandH_ > 8) {
    if (band_[0].createSprite(W_, bandH_) && band_[1].createSprite(W_, bandH_)) break;
    band_[0].deleteSprite();
    band_[1].deleteSprite();
    bandH_ /= 2;
  }
  p_.spr = &band_[0];
  p_.W = W_;
  p_.BH = bandH_;

  hal_.backlight(0);
  // splash
  tft_.fillScreen(cBg_);
  tft_.startWrite();
  for (int by = 0, i = 0; by < H_; by += bandH_, i++) {
    lgfx::LGFX_Sprite& band = band_[i & 1];
    p_.spr = &band;
    p_.oy = by;
    band.fillSprite(cBg_);
    int cy = H_ / 2 - S(24);
    p_.circle(W_ / 2, cy, S(22), cAccent_);
    p_.circle(W_ / 2, cy, S(9), cBg_);
    p_.textCenter(fTitle_, W_ / 2, cy + S(34), "AgentTally", cText_);
    char ver[64];
    snprintf(ver, sizeof ver, "v%s \xC2\xB7 %s", AT_FW_VERSION, hal_.board());
    p_.textCenter(fSmall_, W_ / 2, cy + S(34) + fTitle_->height + S(4), ver, cMuted_);
    pushBand(band, by);
  }
  tft_.waitDMA();
  tft_.endWrite();
  for (int b = 0; b <= cfg_.brightness; b += 8) {
    hal_.backlight(b);
    curBl_ = b;
  }
  hal_.backlight(cfg_.brightness);
  curBl_ = cfg_.brightness;
  lastActivity_ = hal_.ms();
  sendHello();
}

void App::setupPalette() {
  auto c = [this](uint8_t r, uint8_t g, uint8_t b) { return cfg_.swaprb ? rgb(b, g, r) : rgb(r, g, b); };
  cBg_ = c(0x0D, 0x0E, 0x12);
  cCard_ = c(0x1A, 0x1C, 0x23);
  cCardHi_ = c(0x23, 0x26, 0x30);
  cText_ = c(0xEE, 0xEC, 0xE8);
  cMuted_ = c(0x93, 0x98, 0xA3);
  cDim_ = c(0x5A, 0x5F, 0x6B);
  cLine_ = c(0x24, 0x27, 0x30);
  cAccent_ = c(0xD9, 0x77, 0x57);  // warm coral
  cWork_ = c(0xF2, 0xB3, 0x3D);
  cWait_ = c(0xFF, 0x5A, 0x5F);
  cDone_ = c(0x3F, 0xCF, 0x8E);
  cIdle_ = c(0x6B, 0x72, 0x80);
}

// ------------------------------------------------------------------ main loop

void App::tick() {
  pollLink();
  pollTouch();
  uint32_t now = hal_.ms();  // after polling: pollLink() may have stamped lastRx_

  bool wasLinked = linked_;
  linked_ = lastRx_ && (now - lastRx_ < 15000);
  if (wasLinked != linked_) dirty_ = true;
  if (!linked_ && now - lastHello_ > 3000) sendHello();

  int w, d, wk, id;
  counts(w, d, wk, id);
  onStateChange(w, d);
  bool wasSleeping = sleeping_;
  sleeping_ = nightNow() && (int32_t)(now - peekUntil_) >= 0;
  if (wasSleeping && !sleeping_) dirty_ = true;
  updateLed();
  updateBacklight();
  if (sleeping_) return;  // nothing to see: skip drawing

  uint32_t interval = animating() ? 50 : 1000;
  if (dirty_ || now - lastFrame_ >= interval) {
    render();
  }
}

void App::counts(int& waiting, int& done, int& working, int& idle) {
  waiting = done = working = idle = 0;
  for (auto& s : sess_) {
    if (!s.used) continue;
    switch (s.st) {
      case ST_WAITING: waiting++; break;
      case ST_DONE: if (!s.ack) done++; else idle++; break;
      case ST_WORKING: working++; break;
      default: idle++;
    }
  }
}

void App::onStateChange(int waiting, int done) {
  if (waiting > lastWaiting_) {
    lastActivity_ = hal_.ms();
    if (cfg_.sound) { hal_.tone(1760, 70); hal_.tone(2349, 90); }
  } else if (done > lastDone_) {
    lastActivity_ = hal_.ms();
    if (cfg_.sound) hal_.tone(1318, 60);
  }
  lastWaiting_ = waiting;
  lastDone_ = done;
}

static const uint32_t FRAME_PULSE_MS = 1400, FRAME_PULSES = 3;

void App::startAlert(uint16_t color, uint8_t r, uint8_t g, uint8_t b) {
  uint32_t now = hal_.ms();
  beatStart_ = now;
  alertUntil_ = now + FRAME_PULSE_MS * FRAME_PULSES;
  alertRgb_[0] = r; alertRgb_[1] = g; alertRgb_[2] = b;
  frameColor_ = color;
  frameOn_ = cfg_.flash;
  dirty_ = true;
}

// 0 -> 1 -> 0 once per FRAME_PULSE_MS, phase-locked to the last alert.
float App::beat() const {
  float ph = ((hal_.ms() - beatStart_) % FRAME_PULSE_MS) / (float)FRAME_PULSE_MS;
  return 0.5f - 0.5f * cosf(ph * 2 * (float)M_PI);
}

// Soft glow along the screen edges: a few layers fading inwards, pulsing
// FRAME_PULSES times and then gone.
void App::drawFrame() {
  if (!frameOn_) return;
  if ((int32_t)(hal_.ms() - alertUntil_) >= 0) { frameOn_ = false; dirty_ = true; return; }
  float k = beat();
  const int layers = 3;
  int th = S(3);
  for (int i = 0; i < layers; i++) {
    uint8_t a = (uint8_t)(k * (255 - i * 85));
    uint16_t c = mix(cBg_, frameColor_, a);
    int o = i * th;
    p_.fill(o, o, W_ - 2 * o, th, c);                    // top
    p_.fill(o, H_ - o - th, W_ - 2 * o, th, c);          // bottom
    p_.fill(o, o + th, th, H_ - 2 * o - 2 * th, c);      // left
    p_.fill(W_ - o - th, o + th, th, H_ - 2 * o - 2 * th, c);  // right
  }
}

float App::pulse(uint32_t periodMs) const {
  float t = (hal_.ms() % periodMs) / (float)periodMs;
  return 0.5f - 0.5f * cosf(t * 2 * (float)M_PI);
}

bool App::animating() const {
  if (touching_ || ledTestUntil_ || frameOn_ || focusState_ || (int32_t)(hal_.ms() - alertUntil_) < 0) return true;
  for (auto& s : sess_)
    if (s.used && (s.st == ST_WAITING || s.st == ST_WORKING)) return true;
  return false;
}

// The LED also feeds the light bar on the stand's bezel, so it shows every state:
// needs you = red breathing, done = steady green, working = slow dim amber.
// 0 steady, 1 breathe, 2 slow breathe, 3 fast pulse, 4 double blink, 5 blink
float App::ledPattern(int pat) const {
  uint32_t t = hal_.ms();
  switch (pat) {
    case 1: return 0.08f + 0.92f * pulse(2000);
    case 2: return 0.12f + 0.88f * pulse(4000);
    case 3: return 0.05f + 0.95f * pulse(700);
    case 4: { uint32_t p = t % 1600; return (p < 140 || (p >= 300 && p < 440)) ? 1.0f : 0.0f; }
    case 5: return (t % 1600) < 800 ? 1.0f : 0.0f;
    default: return 1.0f;
  }
}

void App::updateLed() {
  if (sleeping_ && !ledTestUntil_) { hal_.led(0, 0, 0); return; }
  if (ledTestUntil_) {
    if ((int32_t)(hal_.ms() - ledTestUntil_) < 0) {
      float k = ledPattern(ledTestPat_);
      hal_.led((uint8_t)(ledTest_[0] * k), (uint8_t)(ledTest_[1] * k), (uint8_t)(ledTest_[2] * k));
      return;
    }
    ledTestUntil_ = 0;
    if (screen_ == SCR_LEDTEST) { screen_ = SCR_LIST; dirty_ = true; }
  }
  if (!cfg_.led) { hal_.led(0, 0, 0); return; }
  int working = 0;
  for (auto& s : sess_)
    if (s.used && s.st == ST_WORKING) working++;
  if ((int32_t)(hal_.ms() - alertUntil_) < 0) {  // in step with the frame
    float k = beat();
    hal_.led((uint8_t)(alertRgb_[0] * k), (uint8_t)(alertRgb_[1] * k), (uint8_t)(alertRgb_[2] * k));
  } else if (lastWaiting_) {
    uint8_t v = 25 + (uint8_t)(230 * beat());
    hal_.led(v, 0, 0);
  } else if (lastDone_) {
    hal_.led(0, 170, 0);
  } else if (working) {
    float k = 0.35f + 0.65f * pulse(3200);
    hal_.led((uint8_t)(150 * k), (uint8_t)(55 * k), 0);
  } else {
    hal_.led(0, 0, 0);
  }
}

// Night hours by the host's local clock; unknown time means never asleep.
bool App::nightNow() const {
  if (!cfg_.sleep || !epochAtSync_ || cfg_.sleepFrom == cfg_.sleepTo) return false;
  int64_t t = epochAtSync_ + (int64_t)(hal_.ms() - syncMs_) / 1000;
  int h = (int)((((t % 86400) + 86400) % 86400) / 3600);
  if (cfg_.sleepFrom < cfg_.sleepTo) return h >= cfg_.sleepFrom && h < cfg_.sleepTo;
  return h >= cfg_.sleepFrom || h < cfg_.sleepTo;  // window across midnight
}

void App::updateBacklight() {
  if (sleeping_) {
    if (curBl_) { curBl_ = 0; hal_.backlight(0); }
    return;
  }
  uint8_t target = cfg_.brightness;
  dimmed_ = cfg_.autodim && !lastWaiting_ && hal_.ms() - lastActivity_ > 180000;
  if (dimmed_) target = cfg_.brightness / 8 > 6 ? cfg_.brightness / 8 : 6;
  if (curBl_ != target) {
    int step = curBl_ < target ? 6 : -2;
    int nb = curBl_ + step;
    if ((step > 0 && nb > target) || (step < 0 && nb < target)) nb = target;
    curBl_ = nb;
    hal_.backlight(curBl_);
  }
}

// ------------------------------------------------------------------ host link

void App::send(const char* json) {
  hal_.write(json, strlen(json));
  hal_.write("\n", 1);
}

void App::sendHello() {
  char b[200];
  snprintf(b, sizeof b, "{\"t\":\"hello\",\"v\":%d,\"fw\":\"%s\",\"board\":\"%s\",\"panel\":\"%s\",\"w\":%d,\"h\":%d}",
           AT_PROTO, AT_FW_VERSION, hal_.board(), hal_.panel(), W_, H_);
  send(b);
  lastHello_ = hal_.ms();
}

void App::sendCfg() {
  char b[300];
  snprintf(b, sizeof b,
           "{\"t\":\"cfg\",\"brightness\":%d,\"autodim\":%d,\"invert\":%d,\"flip\":%d,\"swaprb\":%d,"
           "\"led\":%d,\"sound\":%d,\"gamma\":%d,\"flash\":%d,\"sleep\":%d,\"sleep_from\":%d,\"sleep_to\":%d}",
           cfg_.brightness, cfg_.autodim, cfg_.invert, cfg_.flip, cfg_.swaprb, cfg_.led, cfg_.sound, cfg_.gamma,
           cfg_.flash, cfg_.sleep, cfg_.sleepFrom, cfg_.sleepTo);
  send(b);
}

void App::pollLink() {
  uint8_t buf[256];
  size_t n;
  while ((n = hal_.read(buf, sizeof buf)) > 0) {
    for (size_t i = 0; i < n; i++) {
      char c = (char)buf[i];
      if (c == '\n' || c == '\r') {
        if (lineLen_ && !overflow_) {
          line_[lineLen_] = 0;
          handleLine(line_);
        }
        lineLen_ = 0;
        overflow_ = false;
      } else if (lineLen_ < kLineMax - 1) {
        line_[lineLen_++] = c;
      } else {
        overflow_ = true;
      }
    }
  }
}

Session* App::find(const char* id) {
  for (auto& s : sess_)
    if (s.used && !strcmp(s.id, id)) return &s;
  return nullptr;
}

Session* App::alloc(const char* id) {
  Session* s = find(id);
  if (s) return s;
  for (auto& x : sess_)
    if (!x.used) { s = &x; break; }
  if (!s) {  // full: recycle the oldest idle/acked session
    uint32_t best = 0;
    for (auto& x : sess_) {
      uint32_t age = hal_.ms() - x.sinceMs;
      if (rank(x) >= 2 && age >= best) { best = age; s = &x; }
    }
    if (!s) return nullptr;
  }
  memset(s, 0, sizeof *s);
  s->used = true;
  copyUtf8(s->id, sizeof s->id, id);
  return s;
}

void App::handleLine(char* line) {
  while (*line == ' ') line++;
  if (*line != '{') return;  // boot noise, logs
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;
  const char* t = doc["t"] | "";
  uint32_t now = hal_.ms();
  lastRx_ = now;

  if (doc["tz"].is<int>()) tz_ = doc["tz"].as<int>();
  if (doc["now"].is<int64_t>()) {
    epochAtSync_ = doc["now"].as<int64_t>() + tz_;
    syncMs_ = now;
  }

  if (!strcmp(t, "s")) {
    const char* id = doc["id"] | "";
    if (!*id) return;
    Session* s = alloc(id);
    if (!s) return;
    State st = parseState(doc["st"] | "idle");
    uint32_t age = doc["age"] | 0;
    uint32_t act = doc["act"] | age;
    bool fresh = !s->sinceMs;
    bool changed = fresh || st != s->st;
    if (changed) lastActivity_ = now;
    s->st = st;
    s->sinceMs = now - age * 1000;
    s->actMs = now - act * 1000;
    // New sessions and state changes take their place at once; plain activity
    // only moves a card at the next calm re-sort (see sortSessions).
    if (changed) s->orderMs = s->actMs;
    copyUtf8(s->src, sizeof s->src, doc["src"] | "");
    copyUtf8(s->name, sizeof s->name, doc["name"] | "");
    copyUtf8(s->det, sizeof s->det, doc["det"] | "");
    copyUtf8(s->cwd, sizeof s->cwd, doc["cwd"] | "");
    copyUtf8(s->pr, sizeof s->pr, doc["pr"] | "");
    copyUtf8(s->rp, sizeof s->rp, doc["rp"] | "");
    s->sub = doc["sub"] | 0;
    s->focusable = (doc["fo"] | 0) != 0;
    s->ack = (doc["ack"] | 1) != 0;
    // A session that starts waiting for you, or just finished, raises an alert:
    // frame, LED and its own card pulse together.
    if (changed && (st == ST_WAITING || (st == ST_DONE && !s->ack))) {
      copyUtf8(alertId_, sizeof alertId_, s->id);
      if (st == ST_WAITING) startAlert(cWait_, 255, 0, 0);
      else startAlert(cDone_, 0, 255, 0);
    }
    // A finished session that is already open on screen counts as seen.
    if (screen_ == SCR_DETAIL && !s->ack && !strcmp(detailId_, s->id) && st == ST_DONE) ackSession(s);
    dirty_ = true;
  } else if (!strcmp(t, "d")) {
    Session* s = find(doc["id"] | "");
    if (s) { s->used = false; dirty_ = true; }
  } else if (!strcmp(t, "clr")) {
    for (auto& s : sess_) s.used = false;
    dirty_ = true;
  } else if (!strcmp(t, "hi")) {
    copyUtf8(host_, sizeof host_, doc["host"] | "");
    dirty_ = true;
  } else if (!strcmp(t, "focused")) {
    if (!strcmp(doc["id"] | "", detailId_)) {
      focusState_ = (doc["ok"] | 0) ? 2 : 3;
      copyUtf8(focusMsg_, sizeof focusMsg_, doc["m"] | "");
      focusAt_ = now;
      dirty_ = true;
    }
  } else if (!strcmp(t, "who")) {
    sendHello();
  } else if (!strcmp(t, "cfg")) {
    bool disp = false, pal = false, any = false;
    if (doc["brightness"].is<int>()) { int b = doc["brightness"]; cfg_.brightness = b < 10 ? 10 : b > 255 ? 255 : b; any = true; }
    if (doc["autodim"].is<int>()) { cfg_.autodim = doc["autodim"].as<int>(); any = true; }
    if (doc["invert"].is<int>()) { cfg_.invert = doc["invert"].as<int>(); disp = any = true; }
    if (doc["flip"].is<int>()) { cfg_.flip = doc["flip"].as<int>(); disp = any = true; }
    if (doc["swaprb"].is<int>()) { cfg_.swaprb = doc["swaprb"].as<int>(); pal = any = true; }
    if (doc["led"].is<int>()) { cfg_.led = doc["led"].as<int>(); any = true; }
    if (doc["sound"].is<int>()) { cfg_.sound = doc["sound"].as<int>(); any = true; }
    if (doc["sleep"].is<int>()) { cfg_.sleep = doc["sleep"].as<int>(); any = true; }
    if (doc["sleep_from"].is<int>()) { cfg_.sleepFrom = doc["sleep_from"].as<int>() % 24; any = true; }
    if (doc["sleep_to"].is<int>()) { cfg_.sleepTo = doc["sleep_to"].as<int>() % 24; any = true; }
    if (doc["flash"].is<int>()) { cfg_.flash = doc["flash"].as<int>(); any = true; if (cfg_.flash) startAlert(cAccent_, 255, 90, 40); }
    if (doc["gamma"].is<int>()) { cfg_.gamma = doc["gamma"].as<int>() & 7; disp = any = true; }
    if (doc["beep"].is<int>()) hal_.tone(1760, 120);
    if (doc["panel"].is<int>()) hal_.setPanel(doc["panel"].as<int>());
    if (doc["test"].is<int>()) { screen_ = doc["test"].as<int>() ? SCR_TEST : SCR_LIST; dirty_ = true; }
    if (doc["led_rgb"].is<JsonArrayConst>()) {
      JsonArrayConst c = doc["led_rgb"].as<JsonArrayConst>();
      for (int i = 0; i < 3; i++) ledTest_[i] = c[i] | 0;
      ledTestPat_ = doc["led_pat"] | 0;
      ledTestUntil_ = now + (uint32_t)(doc["led_ms"] | 20000);
      copyUtf8(ledTestLabel_, sizeof ledTestLabel_, doc["led_label"] | "");
      screen_ = SCR_LEDTEST;
      dirty_ = true;
    } else if (doc["led_rgb"].is<int>()) {  // led_rgb: 0 ends the test
      ledTestUntil_ = now;
    }
    if (disp) hal_.applyDisplay(cfg_);
    if (pal) setupPalette();
    if (any) { hal_.save(cfg_); lastActivity_ = now; dirty_ = true; }
    sendCfg();
  }
}

void App::ackSession(Session* s) {
  if (!s || s->ack) return;
  s->ack = true;
  char b[80];
  snprintf(b, sizeof b, "{\"t\":\"ack\",\"id\":\"%s\"}", s->id);
  send(b);
}

// ------------------------------------------------------------------ touch

void App::pollTouch() {
  int32_t x, y;
  bool down = tft_.getTouch(&x, &y) > 0;
  uint32_t now = hal_.ms();
  if (down && !touching_) {
    touching_ = true;
    dragged_ = false;
    touchX0_ = x; touchY0_ = touchY_ = y;
    scroll0_ = scroll_;
    touchT0_ = now;
    wakeTouch_ = dimmed_ || sleeping_;  // first touch on a dimmed or sleeping screen only wakes it
    if (sleeping_) { peekUntil_ = now + 30000; frameOn_ = false; }
    lastActivity_ = now;
  } else if (down && touching_) {
    touchY_ = y;
    int dy = touchY_ - touchY0_;
    if (!dragged_ && (dy > S(10) || dy < -S(10))) dragged_ = true;
    if (dragged_ && screen_ == SCR_LIST) {
      int ns = scroll0_ - dy;
      if (ns < 0) ns = 0;
      if (ns > scrollMax_) ns = scrollMax_;
      if (ns != scroll_) { scroll_ = ns; dirty_ = true; }
    }
    if (!dragged_ && !wakeTouch_ && touchT0_ && now - touchT0_ > 900) {
      touchT0_ = 0;  // fire once
      onLongPress(touchX0_, touchY0_);
    }
  } else if (!down && touching_) {
    touching_ = false;
    lastActivity_ = now;
    if (!dragged_ && !wakeTouch_ && touchT0_) onTap(touchX0_, touchY0_);
    dirty_ = true;
  }
}

void App::tapAt(int x, int y) { onTap(x, y); render(); }

void App::onLongPress(int x, int y) {
  if (screen_ == SCR_LIST && y < listTop()) {
    screen_ = SCR_SETTINGS;
    dirty_ = true;
  }
}

void App::onTap(int x, int y) {
  dirty_ = true;
  if (screen_ == SCR_LIST) {
    if (y < listTop()) {
      if (x > W_ - S(40)) screen_ = SCR_SETTINGS;
      return;
    }
    if (y >= listBottom()) return;
    sortSessions();
    int idx = (y - listTop() + scroll_ - S(4)) / (cardH() + cardGap());
    int within = (y - listTop() + scroll_ - S(4)) % (cardH() + cardGap());
    if (idx >= 0 && idx < count_ && within < cardH()) {
      Session& s = sess_[order_[idx]];
      copyUtf8(detailId_, sizeof detailId_, s.id);
      screen_ = SCR_DETAIL;
      focusState_ = 0;
      if (s.st == ST_DONE) ackSession(&s);
    }
  } else if (screen_ == SCR_TEST) {
    screen_ = SCR_SETTINGS;
  } else if (screen_ == SCR_LEDTEST) {
    ledTestUntil_ = hal_.ms();
  } else if (screen_ == SCR_DETAIL) {
    if (y < S(40)) {
      Session* s = find(detailId_);
      if (s && s->focusable && focusBtnX_ && x >= focusBtnX_ - S(8)) {
        char b[80];
        snprintf(b, sizeof b, "{\"t\":\"focus\",\"id\":\"%s\"}", s->id);
        send(b);
        focusState_ = 1;
        focusAt_ = hal_.ms();
      } else if (x < W_ / 2) {
        screen_ = SCR_LIST;
        focusState_ = 0;
      }
    }
  } else if (screen_ == SCR_SETTINGS) {
    if (y < S(40)) { screen_ = SCR_LIST; return; }
    int row = (y - S(42)) / S(26);
    bool disp = false, pal = false;
    switch (row) {
      case 0:
        if (x < W_ - S(70)) break;
        if (x < W_ - S(40)) cfg_.brightness = cfg_.brightness > 40 ? cfg_.brightness - 30 : 10;
        else cfg_.brightness = cfg_.brightness < 225 ? cfg_.brightness + 30 : 255;
        break;
      case 1: cfg_.autodim = !cfg_.autodim; break;
      case 2: cfg_.invert = !cfg_.invert; disp = true; break;
      case 3: cfg_.flash = !cfg_.flash; if (cfg_.flash) startAlert(cAccent_, 255, 90, 40); break;
      case 4: cfg_.flip = !cfg_.flip; disp = true; break;
      case 5: cfg_.led = !cfg_.led; break;
      case 6: cfg_.sound = !cfg_.sound; if (cfg_.sound) hal_.tone(1760, 80); break;
      case 7: cfg_.sleep = !cfg_.sleep; break;
      case 8: screen_ = SCR_TEST; return;
      case 9: hal_.calibrateTouch(cfg_); break;
      default: return;
    }
    if (disp) hal_.applyDisplay(cfg_);
    if (pal) setupPalette();
    hal_.save(cfg_);
    sendCfg();
  }
}

// ------------------------------------------------------------------ rendering

void App::sortSessions() {
  count_ = 0;
  for (int i = 0; i < MAX_SESSIONS; i++)
    if (sess_[i].used) order_[count_++] = i;
  uint32_t now = hal_.ms();
  if (now - lastResort_ >= 30000) {
    lastResort_ = now;
    for (auto& s : sess_) s.orderMs = s.actMs;
  }
  // Waiting sessions: whoever has waited longest first (answer them in order).
  // Everything else: most recently active first, refreshed every 30 s.
  for (int i = 1; i < count_; i++) {  // insertion sort, n <= 24
    int v = order_[i], j = i - 1;
    while (j >= 0) {
      const Session &a = sess_[order_[j]], &b = sess_[v];
      int ra = rank(a), rb = rank(b);
      bool after;
      if (ra != rb) after = ra > rb;
      else if (a.st == ST_WAITING) after = (now - a.sinceMs) < (now - b.sinceMs);
      else after = (now - a.orderMs) > (now - b.orderMs);
      if (!after) break;
      order_[j + 1] = order_[j];
      j--;
    }
    order_[j + 1] = v;
  }
}

void App::render() {
  lastFrame_ = hal_.ms();
  dirty_ = false;
  sortSessions();
  int contentH = count_ * (cardH() + cardGap()) + S(4);
  scrollMax_ = contentH - (listBottom() - listTop());
  if (scrollMax_ < 0) scrollMax_ = 0;
  if (scroll_ > scrollMax_) scroll_ = scrollMax_;

  tft_.startWrite();
  for (int by = 0, i = 0; by < H_; by += bandH_, i++) {
    // Band i is drawn while band i-1 is still on its way to the panel. Its buffer
    // was last used by band i-2, which finished before band i-1 could start.
    lgfx::LGFX_Sprite& band = band_[i & 1];
    p_.spr = &band;
    p_.oy = by;
    band.fillSprite(cBg_);
    drawScreen();
    drawFrame();
    pushBand(band, by);
  }
  tft_.waitDMA();
  tft_.endWrite();
}

void App::pushBand(lgfx::LGFX_Sprite& band, int y) {
  int h = H_ - y < bandH_ ? H_ - y : bandH_;
  tft_.pushImageDMA(0, y, W_, h, (const lgfx::swap565_t*)band.getBuffer());
}

void App::drawScreen() {
  switch (screen_) {
    case SCR_LIST: {
      int w, d, wk, id;
      counts(w, d, wk, id);
      if (count_) drawList(); else drawEmpty();
      drawHeader(w, d, wk);
      drawFooter();
      break;
    }
    case SCR_DETAIL:
      if (!find(detailId_)) { screen_ = SCR_LIST; dirty_ = true; return; }
      drawDetail();
      break;
    case SCR_SETTINGS:
      drawSettings();
      break;
    case SCR_TEST:
      drawTest();
      break;
    case SCR_LEDTEST:
      drawLedTest();
      break;
  }
}

uint16_t App::stateColor(State st) const {
  switch (st) {
    case ST_WAITING: return cWait_;
    case ST_DONE: return cDone_;
    case ST_WORKING: return cWork_;
    default: return cIdle_;
  }
}

const char* App::stateLabel(State st) const {
  switch (st) {
    case ST_WAITING: return "NEEDS YOU";
    case ST_DONE: return "DONE";
    case ST_WORKING: return "WORKING";
    default: return "IDLE";
  }
}

void App::ageText(uint32_t sinceMs, char* out, size_t n) const {
  uint32_t s = (hal_.ms() - sinceMs) / 1000;
  if (s < 10) snprintf(out, n, "now");
  else if (s < 60) snprintf(out, n, "%us", (unsigned)s);
  else if (s < 3600) snprintf(out, n, "%um", (unsigned)(s / 60));
  else if (s < 86400) snprintf(out, n, "%uh", (unsigned)(s / 3600));
  else snprintf(out, n, "%ud", (unsigned)(s / 86400));
}

void App::drawHeader(int waiting, int done, int working) {
  int hh = S(36);
  if (!p_.vis(0, hh + 1)) return;
  uint16_t bg = waiting ? mix(cBg_, cWait_, 26 + (uint8_t)(34 * beat())) : cBg_;
  p_.fill(0, 0, W_, hh, bg);
  p_.hline(0, hh, W_, waiting ? mix(cLine_, cWait_, 140) : cLine_);

  int ty = (hh - fTitle_->height) / 2;
  if (waiting) {
    char b[32];
    snprintf(b, sizeof b, waiting == 1 ? "1 needs you" : "%d need you", waiting);
    p_.text(fTitle_, S(12), ty, b, cText_);
  } else {
    p_.text(fTitle_, S(12), ty, "Agents", cText_);
  }

  // kebab menu (settings)
  int kx = W_ - S(16);
  for (int i = -1; i <= 1; i++) p_.circle(kx, hh / 2 + i * S(5), S(1) + 1, cMuted_);

  int x = W_ - S(34);
  int cy = hh / 2;
  int ny = (hh - fBold_->height) / 2;
  if (!linked_) {
    int w = Painter::width(fSmall_, "offline");
    p_.text(fSmall_, x - w, (hh - fSmall_->height) / 2, "offline", cMuted_);
    p_.circle(x - w - S(9), cy, S(3), cDim_);
    return;
  }
  struct { int n; uint16_t c; } chips[3] = {{working, cWork_}, {done, cDone_}, {waiting, cWait_}};
  for (auto& ch : chips) {
    if (!ch.n || (ch.c == cWait_ && waiting)) continue;  // waiting is already in the title
    char b[8];
    snprintf(b, sizeof b, "%d", ch.n);
    int w = Painter::width(fBold_, b);
    p_.text(fBold_, x - w, ny, b, cText_);
    p_.circle(x - w - S(8), cy, S(4), ch.c);
    x -= w + S(22);
  }
}

void App::drawFooter() {
  int y = listBottom();
  if (!p_.vis(y, H_ - y)) return;
  p_.fill(0, y, W_, H_ - y, cBg_);
  int ty = y + (H_ - y - fSmall_->height) / 2;
  p_.circle(S(14), y + (H_ - y) / 2, S(3), linked_ ? cDone_ : cDim_);
  const char* label = linked_ ? (host_[0] ? host_ : "connected") : "no bridge";
  p_.text(fSmall_, S(22), ty, label, cMuted_, W_ / 2);
  if (epochAtSync_) {
    int64_t t = epochAtSync_ + (int64_t)(hal_.ms() - syncMs_) / 1000;
    int sec = (int)(((t % 86400) + 86400) % 86400);
    char b[8];
    snprintf(b, sizeof b, "%02d:%02d", sec / 3600, (sec / 60) % 60);
    p_.textRight(fSmall_, W_ - S(12), ty, b, cMuted_);
  }
}

void App::drawStatusIcon(State st, int cx, int cy, int r) {
  switch (st) {
    case ST_WORKING: {
      float a = (hal_.ms() % 1200) * 360.0f / 1200.0f;
      p_.ring(cx, cy, r - S(2), r, 0, 360, mix(cCard_, cWork_, 60));
      p_.ring(cx, cy, r - S(2), r, a, a + 100, cWork_);
      break;
    }
    case ST_WAITING: {
      float k = beat();
      p_.circle(cx, cy, r + (int)(S(3) * k), mix(cCard_, cWait_, 70));
      p_.circle(cx, cy, r - 1, cWait_);
      break;
    }
    case ST_DONE:
      p_.circle(cx, cy, r, cDone_);
      p_.line(cx - r / 2, cy, cx - r / 6, cy + r / 3, 0.9f, cCard_);
      p_.line(cx - r / 6, cy + r / 3, cx + r / 2, cy - r / 3, 0.9f, cCard_);
      break;
    default:
      p_.ring(cx, cy, r - S(2), r, 0, 360, cIdle_);
  }
}

void App::drawCard(const Session& s, int y, int h) {
  int x = S(8), w = W_ - S(16);
  uint16_t bg = cCard_;
  if (s.st == ST_WAITING) bg = mix(cCard_, cWait_, 22 + (uint8_t)(22 * beat()));
  else if (s.st == ST_DONE && !s.ack) bg = mix(cCard_, cDone_, 24);
  bool alerting = (int32_t)(hal_.ms() - alertUntil_) < 0 && !strcmp(alertId_, s.id);
  if (alerting) {
    // glowing outline + tinted card, on the shared beat
    float k = beat();
    p_.rrect(x - S(3), y - S(3), w + S(6), h + S(6), S(13), mix(cBg_, frameColor_, (uint8_t)(255 * k)));
    bg = mix(bg, frameColor_, (uint8_t)(70 * k));
  }
  p_.rrect(x, y, w, h, S(10), bg);
  uint16_t col = stateColor(s.st);
  p_.rrect(x + S(6), y + S(10), S(4), h - S(20), S(2), col);

  // row 1: name, badges, age
  char age[12];
  ageText(s.sinceMs, age, sizeof age);
  int ageW = Painter::width(fSmall_, age);
  int nx = x + S(18);
  int ny = y + S(9);
  char badge[24] = "";
  if (s.sub) snprintf(badge, sizeof badge, "+%u", s.sub);
  bool srcBadge = s.src[0] && strcmp(s.src, "claude") != 0;
  int badgeW = 0;
  if (badge[0]) badgeW += Painter::width(fSmall_, badge) + S(12);
  if (srcBadge) badgeW += Painter::width(fSmall_, s.src) + S(14);
  int maxName = w - S(18) - ageW - S(14) - badgeW;
  int used = p_.text(fBold_, nx, ny, s.name, cText_, maxName);
  int bx = nx + used + S(6);
  int bh = fSmall_->height + 2;
  int by = ny + (fBold_->height - bh) / 2;
  uint16_t chipBg = mix(bg, cText_, 28);
  if (srcBadge) {
    int bw = Painter::width(fSmall_, s.src) + S(10);
    p_.rrect(bx, by, bw, bh, bh / 2, chipBg);
    p_.text(fSmall_, bx + S(5), by + 1, s.src, cMuted_);
    bx += bw + S(4);
  }
  if (badge[0]) {
    int bw = Painter::width(fSmall_, badge) + S(10);
    p_.rrect(bx, by, bw, bh, bh / 2, mix(bg, cWork_, 60));
    p_.text(fSmall_, bx + S(5), by + 1, badge, cText_);
  }
  p_.textRight(fSmall_, x + w - S(10), ny + (fBold_->height - fSmall_->height) / 2, age, cMuted_);

  // row 2: status icon + detail
  int r = S(6);
  int cy = y + S(39);
  drawStatusIcon(s.st, nx + r, cy, r);
  uint16_t dc = s.st == ST_WAITING ? mix(cMuted_, cText_, 200) : cMuted_;
  const char* det = s.det[0] ? s.det : stateLabel(s.st);
  p_.text(fBody_, nx + 2 * r + S(7), cy - fBody_->height / 2 - 1, det, dc, w - S(18) - 2 * r - S(17));
}

void App::drawList() {
  int top = listTop(), y = top + S(4) - scroll_;
  for (int i = 0; i < count_; i++, y += cardH() + cardGap()) {
    if (y + cardH() < top || y > listBottom()) continue;
    if (!p_.vis(y, cardH())) continue;
    drawCard(sess_[order_[i]], y, cardH());
  }
  if (scrollMax_ > 0) {  // scrollbar
    int viewH = listBottom() - top;
    int barH = viewH * viewH / (viewH + scrollMax_);
    int barY = top + (viewH - barH) * scroll_ / scrollMax_;
    p_.rrect(W_ - S(4), barY, S(3), barH, 1, cDim_);
  }
}

void App::drawEmpty() {
  int cy = (listTop() + listBottom()) / 2 - S(30);
  int cx = W_ / 2;
  uint16_t ring = linked_ ? cDim_ : cLine_;
  p_.ring(cx, cy, S(24), S(27), 0, 360, ring);
  if (linked_) {
    float k = pulse(3000);
    p_.circle(cx, cy, S(6) + (int)(S(3) * k), mix(cBg_, cAccent_, 120));
    p_.circle(cx, cy, S(5), cAccent_);
  } else {
    p_.circle(cx, cy, S(5), cDim_);
  }
  int y = cy + S(42);
  p_.textCenter(fBold_, cx, y, linked_ ? "No active agents" : "Waiting for the bridge", cText_);
  y += fBold_->height + S(6);
  if (linked_) {
    p_.textCenter(fSmall_, cx, y, "Start a Claude Code session", cMuted_);
    p_.textCenter(fSmall_, cx, y + fSmall_->height + 2, "and it shows up here.", cMuted_);
  } else {
    p_.textCenter(fSmall_, cx, y, "Run on your computer:", cMuted_);
    p_.textCenter(fSmall_, cx, y + fSmall_->height + S(4), "agenttally.py serve", cAccent_);
  }
}

void App::drawBack(const char* title) {
  int hh = S(36);
  p_.fill(0, 0, W_, hh, cBg_);
  p_.hline(0, hh, W_, cLine_);
  int cy = hh / 2;
  p_.line(S(18), cy - S(6), S(12), cy, 1.2f, cText_);
  p_.line(S(12), cy, S(18), cy + S(6), 1.2f, cText_);
  p_.text(fBody_, S(24), (hh - fBody_->height) / 2, "Back", cText_);
  if (title) p_.textCenter(fBold_, W_ / 2, (hh - fBold_->height) / 2, title, cText_);
}

void App::drawDetail() {
  Session* sp = find(detailId_);
  if (!sp) return;
  Session& s = *sp;
  drawBack(nullptr);
  int hh = S(36);
  focusBtnX_ = 0;
  if (s.focusable) {
    // Focus: bring this session's terminal tab to the front on the computer
    uint32_t age = hal_.ms() - focusAt_;
    if ((focusState_ == 1 && age > 8000) || (focusState_ >= 2 && age > 2500)) focusState_ = 0;
    const char* lab = focusState_ == 1 ? "\xE2\x80\xA6" : focusState_ == 2 ? "Opened" : focusState_ == 3 ? focusMsg_ : "Focus";
    if (focusState_ == 3 && !focusMsg_[0]) lab = "Not found";
    uint16_t bg = focusState_ == 2 ? cDone_ : focusState_ == 3 ? cWait_ : cAccent_;
    int bw = Painter::width(fBold_, lab) + S(26), bh = S(26);
    if (bw < S(76)) bw = S(76);
    int bx = W_ - S(10) - bw, by = (hh - bh) / 2;
    focusBtnX_ = bx;
    p_.rrect(bx, by, bw, bh, bh / 2, bg);
    p_.textCenter(fBold_, bx + bw / 2, by + (bh - fBold_->height) / 2, lab, rgb(255, 255, 255));
  } else {
    uint16_t col = stateColor(s.st);
    const char* lab = stateLabel(s.st);
    int lw = Painter::width(fSmall_, lab) + S(16);
    int ph = fSmall_->height + S(6);
    int px = W_ - S(12) - lw, py = (hh - ph) / 2;
    float k = s.st == ST_WAITING ? beat() : 0;
    p_.rrect(px, py, lw, ph, ph / 2, mix(cBg_, col, 70 + (uint8_t)(50 * k)));
    p_.text(fSmall_, px + S(8), py + S(3), lab, cText_);
  }

  int x = S(12), w = W_ - S(24);
  int y = hh + S(10);
  y += p_.wrap(fTitle_, x, y, w, s.name, cText_, 2);
  if (s.cwd[0]) y += p_.wrap(fSmall_, x, y, w, s.cwd, cMuted_, 2);
  y += S(8);

  int footer = H_ - S(24);
  auto section = [&](const char* label, const char* body, int maxLines, uint16_t bc) {
    if (!body || !body[0] || y > footer - S(30)) return;
    p_.hline(x, y, w, cLine_);
    y += S(6);
    p_.text(fSmall_, x, y, label, cDim_);
    y += fSmall_->height + S(2);
    int lh = fBody_->height + 2;
    int room = (footer - y) / lh;
    if (room < 1) return;
    if (maxLines > room) maxLines = room;
    y += p_.wrap(fBody_, x, y, w, body, bc, maxLines) + S(6);
  };
  section("NOW", s.det, 3, s.st == ST_WAITING ? cText_ : mix(cMuted_, cText_, 160));
  section("PROMPT", s.pr, 3, mix(cMuted_, cText_, 160));
  section("LAST REPLY", s.rp, 12, mix(cMuted_, cText_, 160));

  char meta[96], age[12];
  ageText(s.sinceMs, age, sizeof age);
  int n = snprintf(meta, sizeof meta, "%s %s", stateLabel(s.st), age);
  if (s.sub) n += snprintf(meta + n, sizeof meta - n, " \xC2\xB7 %u subagent%s", s.sub, s.sub > 1 ? "s" : "");
  if (s.src[0]) snprintf(meta + n, sizeof meta - n, " \xC2\xB7 %s", s.src);
  p_.fill(0, footer, W_, H_ - footer, cBg_);
  p_.hline(0, footer, W_, cLine_);
  p_.text(fSmall_, x, footer + (H_ - footer - fSmall_->height) / 2, meta, cMuted_, w);
}

void App::drawToggle(int x, int y, bool on) {
  int w = S(34), h = S(18);
  p_.rrect(x, y, w, h, h / 2, on ? cDone_ : cDim_);
  int r = h / 2 - S(2);
  p_.circle(on ? x + w - h / 2 : x + h / 2, y + h / 2, r, cText_);
}

void App::drawSettings() {
  char sleepLabel[32];
  snprintf(sleepLabel, sizeof sleepLabel, "Sleep %02d\xE2\x80\x93%02d", cfg_.sleepFrom, cfg_.sleepTo);
  drawBack("Settings");
  struct Row { const char* label; int kind; bool on; };  // kind 0 toggle, 1 brightness, 2 button
  Row rows[] = {
      {"Brightness", 1, false},        {"Dim when quiet", 0, cfg_.autodim}, {"Invert colors", 0, cfg_.invert},
      {"Flash on alerts", 0, cfg_.flash}, {"Rotate 180\xC2\xB0", 0, cfg_.flip}, {"Status LED", 0, cfg_.led},
      {"Sound", 0, cfg_.sound},        {sleepLabel, 0, cfg_.sleep},
      {"Display test", 2, false},       {"Calibrate touch", 2, false},
  };
  int rh = S(26);
  int y = S(42);
  for (auto& r : rows) {
    if (p_.vis(y, rh)) {
      p_.text(fBody_, S(14), y + (rh - fBody_->height) / 2, r.label, cText_);
      int cy = y + rh / 2;
      if (r.kind == 0) {
        drawToggle(W_ - S(14) - S(34), cy - S(9), r.on);
      } else if (r.kind == 1) {
        char b[8];
        snprintf(b, sizeof b, "%d%%", cfg_.brightness * 100 / 255);
        int bs = S(24);
        int xp = W_ - S(14) - bs, xm = W_ - S(70);
        p_.rrect(xm, cy - bs / 2, bs, bs, S(6), cCard_);
        p_.rrect(xp, cy - bs / 2, bs, bs, S(6), cCard_);
        p_.fill(xm + S(7), cy, bs - S(14), S(2), cText_);
        p_.fill(xp + S(7), cy, bs - S(14), S(2), cText_);
        p_.fill(xp + bs / 2 - S(1), cy - S(5), S(2), S(12), cText_);
        p_.textRight(fSmall_, xm - S(8), cy - fSmall_->height / 2, b, cMuted_);
      } else {
        int bw = Painter::width(fSmall_, "Start") + S(16);
        int bh = S(22);
        p_.rrect(W_ - S(14) - bw, cy - bh / 2, bw, bh, S(6), cCard_);
        p_.text(fSmall_, W_ - S(14) - bw + S(8), cy - fSmall_->height / 2, "Start", cText_);
      }
      p_.hline(S(14), y + rh - 1, W_ - S(28), cLine_);
    }
    y += rh;
  }
}

// Raw colour bars (not run through the palette) to diagnose panel settings:
// white background = inverted, red/blue swapped = BGR, grey ramp = gamma.
void App::drawTest() {
  struct Bar { const char* name; uint16_t c; } bars[] = {
      {"red", 0xF800}, {"green", 0x07E0}, {"blue", 0x001F}, {"white", 0xFFFF},
      {"grey 50%", rgb(128, 128, 128)}, {"grey 25%", rgb(64, 64, 64)}, {"black", 0x0000},
  };
  int n = sizeof bars / sizeof bars[0];
  int top = S(24), bh = (H_ - top) / n;
  p_.fill(0, 0, W_, top, 0x0000);
  char title[64];
  snprintf(title, sizeof title, "%s \xC2\xB7 gamma %d \xC2\xB7 tap to exit", hal_.panel(), cfg_.gamma);
  p_.textCenter(fSmall_, W_ / 2, (top - fSmall_->height) / 2, title, 0xFFFF);
  for (int i = 0; i < n; i++) {
    int y = top + i * bh;
    p_.fill(0, y, W_, i == n - 1 ? H_ - y : bh, bars[i].c);
    uint16_t tc = (i == 3 || i == 4) ? 0x0000 : 0xFFFF;
    p_.text(fBody_, S(10), y + (bh - fBody_->height) / 2, bars[i].name, tc);
  }
}

// Big label + swatch so the person at the desk can say "number 3 looked best".
void App::drawLedTest() {
  static const char* pats[] = {"steady", "breathe", "slow breathe", "fast pulse", "double blink", "blink"};
  p_.textCenter(fSmall_, W_ / 2, S(16), "LED TEST \xC2\xB7 tap to stop", cMuted_);
  uint16_t sw = rgb(ledTest_[0], ledTest_[1], ledTest_[2]);
  if (cfg_.swaprb) sw = rgb(ledTest_[2], ledTest_[1], ledTest_[0]);
  int cy = H_ / 2 - S(30);
  float k = ledPattern(ledTestPat_);
  p_.circle(W_ / 2, cy, S(46), cLine_);
  p_.circle(W_ / 2, cy, S(44), mix(cBg_, sw, (uint8_t)(40 + 215 * k)));
  p_.textCenter(fHuge_, W_ / 2, cy + S(58), ledTestLabel_, cText_);
  char b[48];
  snprintf(b, sizeof b, "%s \xC2\xB7 %d %d %d", pats[ledTestPat_ < 6 ? ledTestPat_ : 0], ledTest_[0], ledTest_[1], ledTest_[2]);
  p_.textCenter(fSmall_, W_ / 2, cy + S(58) + fHuge_->height + S(6), b, cMuted_);
}
