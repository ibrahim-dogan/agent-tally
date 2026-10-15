// Desktop simulator: same App code, SDL2 window, talks to the bridge over TCP
// (agenttally.py serve listens on 127.0.0.1:47322 for simulators).
//
//   .pio/build/sim/program [--port 47322] [--scale 2]
//        [--tap x,y@ms]... [--shot file.ppm@ms]... [--exit ms]
#include <LovyanGFX.hpp>
#include <LGFX_AUTODETECT.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../app.h"

#ifndef SIM_W
#define SIM_W 240
#endif
#ifndef SIM_H
#define SIM_H 320
#endif

static int g_port = 47322;
static int g_scale = 2;
struct Tap { int x, y; uint32_t at; bool done; };
struct Shot { std::string file; uint32_t at; bool done; };
static std::vector<Tap> g_taps;
static std::vector<Shot> g_shots;
static uint32_t g_exit = 0;

static LGFX* g_tft;

class SimHal : public Hal {
 public:
  int fd = -1;
  uint32_t nextConnect = 0;
  uint8_t lr = 0, lg = 0, lb = 0;
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

  uint32_t ms() override {
    return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
        .count();
  }

  void ensure() {
    if (fd >= 0 || ms() < nextConnect) return;
    nextConnect = ms() + 2000;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(g_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (sockaddr*)&a, sizeof a) != 0) { close(s); return; }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    fd = s;
    printf("[sim] connected to bridge on :%d\n", g_port);
  }

  size_t read(uint8_t* buf, size_t max) override {
    ensure();
    if (fd < 0) return 0;
    ssize_t n = recv(fd, buf, max, 0);
    if (n > 0) return (size_t)n;
    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) { close(fd); fd = -1; }
    return 0;
  }

  void write(const char* s, size_t n) override {
    ensure();
    if (fd < 0) return;
    if (send(fd, s, n, 0) < 0 && errno != EAGAIN) { close(fd); fd = -1; }
  }

  void led(uint8_t r, uint8_t g, uint8_t b) override {
    bool on = r | g | b, was = lr | lg | lb;
    if (on != was || (on && ((r > 0) != (lr > 0) || (g > 0) != (lg > 0)))) {
      printf("[sim] LED %s\n", !on ? "off" : r ? "red (pulsing)" : g ? "green" : "blue");
    }
    lr = r; lg = g; lb = b;
  }

  void backlight(uint8_t v) override { g_tft->setBrightness(v); }
  void tone(uint16_t hz, uint16_t ms) override { printf("[sim] beep %u Hz\n", hz); }

  std::string path() {
    const char* h = getenv("HOME");
    return std::string(h ? h : ".") + "/.agent-tally/sim-settings.bin";
  }
  void load(Settings& s) override {
    FILE* f = fopen(path().c_str(), "rb");
    if (!f) return;
    Settings t;
    if (fread(&t, sizeof t, 1, f) == 1 && t.magic == s.magic && t.version == s.version) s = t;
    fclose(f);
  }
  void save(const Settings& s) override {
    FILE* f = fopen(path().c_str(), "wb");
    if (!f) return;
    fwrite(&s, sizeof s, 1, f);
    fclose(f);
  }
  void applyDisplay(const Settings& s) override { g_tft->setRotation(s.flip ? 2 : 0); }
  const char* board() override { return "sim"; }
  const char* panel() override { return "sdl"; }
};

static void saveShot(const std::string& file) {
  int w = g_tft->width(), h = g_tft->height();
  std::vector<lgfx::rgb888_t> px(w * h);
  g_tft->readRect(0, 0, w, h, px.data());
  FILE* f = fopen(file.c_str(), "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", w, h);
  for (auto& p : px) { uint8_t c[3] = {p.r, p.g, p.b}; fwrite(c, 3, 1, f); }
  fclose(f);
  printf("[sim] saved %s\n", file.c_str());
}

static int user_func(bool* running) {
  SimHal hal;
  App app(*g_tft, hal);
  g_tft->init();
  app.begin();
  while (*running) {
    app.tick();
    uint32_t now = hal.ms();
    for (auto& t : g_taps)
      if (!t.done && now >= t.at) { t.done = true; app.tapAt(t.x, t.y); }
    for (auto& s : g_shots)
      if (!s.done && now >= s.at) { s.done = true; app.renderNow(); saveShot(s.file); }
    if (g_exit && now >= g_exit) { fflush(stdout); exit(0); }
    lgfx::delay(5);
  }
  return 0;
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--port") g_port = atoi(next().c_str());
    else if (a == "--scale") g_scale = atoi(next().c_str());
    else if (a == "--exit") g_exit = atoi(next().c_str());
    else if (a == "--tap") {
      std::string v = next();
      Tap t{};
      sscanf(v.c_str(), "%d,%d@%u", &t.x, &t.y, &t.at);
      g_taps.push_back(t);
    } else if (a == "--shot") {
      std::string v = next();
      auto at = v.rfind('@');
      g_shots.push_back({v.substr(0, at), (uint32_t)(at == std::string::npos ? 3000 : atoi(v.c_str() + at + 1)), false});
    }
  }
  static LGFX tft(SIM_W, SIM_H, g_scale);
  g_tft = &tft;
  return lgfx::Panel_sdl::main(user_func);
}
