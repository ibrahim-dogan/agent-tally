#pragma once
// Band renderer: the UI is drawn into a short full-width sprite ("band") several
// times per frame, each time shifted by `oy`, and every band is pushed to the
// panel. That keeps RAM low (no full framebuffer) and the screen flicker-free.
#include <LovyanGFX.hpp>
#include "font.h"

static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static inline uint16_t mix(uint16_t a, uint16_t b, uint8_t t) {  // t=0 -> a, 255 -> b
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  int r = ar + (((br - ar) * t) >> 8);
  int g = ag + (((bg - ag) * t) >> 8);
  int bl = ab + (((bb - ab) * t) >> 8);
  return (r << 11) | (g << 5) | bl;
}

struct Painter {
  lgfx::LGFX_Sprite* spr = nullptr;
  int oy = 0;   // band top in screen coordinates
  int W = 0;    // band width (= screen width)
  int BH = 0;   // band height

  // True if the rows [y, y+h) intersect the current band.
  bool vis(int y, int h) const { return y + h > oy && y < oy + BH; }

  void fill(int x, int y, int w, int h, uint16_t c) {
    if (vis(y, h)) spr->fillRect(x, y - oy, w, h, c);
  }
  void rrect(int x, int y, int w, int h, int r, uint16_t c) {
    if (vis(y, h)) spr->fillSmoothRoundRect(x, y - oy, w, h, r, c);
  }
  void tri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) {
    int top = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int bot = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    if (vis(top, bot - top + 1)) spr->fillTriangle(x0, y0 - oy, x1, y1 - oy, x2, y2 - oy, c);
  }
  void circle(int cx, int cy, int r, uint16_t c) {
    if (vis(cy - r, 2 * r + 1)) spr->fillSmoothCircle(cx, cy - oy, r, c);
  }
  void ring(int cx, int cy, int r0, int r1, float a0, float a1, uint16_t c) {
    if (vis(cy - r1, 2 * r1 + 1)) spr->fillArc(cx, cy - oy, r0, r1, a0, a1, c);
  }
  void line(int x0, int y0, int x1, int y1, float wdt, uint16_t c) {
    int top = y0 < y1 ? y0 : y1, h = (y0 < y1 ? y1 - y0 : y0 - y1) + 1;
    if (vis(top - (int)wdt - 1, h + 2 * (int)wdt + 2)) spr->drawWideLine(x0, y0 - oy, x1, y1 - oy, wdt, c);
  }
  void hline(int x, int y, int w, uint16_t c) { fill(x, y, w, 1, c); }

  // ---- text ---------------------------------------------------------------
  static uint32_t nextCp(const char*& s);
  static const AtGlyph* glyph(const AtFont* f, uint32_t cp);
  static int width(const AtFont* f, const char* s, int n = -1);

  // Draw a UTF-8 string with its line top at y. Truncates with an ellipsis when
  // maxw > 0 and the text is wider. Returns the pen advance.
  int text(const AtFont* f, int x, int y, const char* s, uint16_t c, int maxw = 0);
  int textRight(const AtFont* f, int xr, int y, const char* s, uint16_t c) {
    int w = width(f, s);
    text(f, xr - w, y, s, c);
    return w;
  }
  int textCenter(const AtFont* f, int cx, int y, const char* s, uint16_t c) {
    int w = width(f, s);
    text(f, cx - w / 2, y, s, c);
    return w;
  }
  // Word-wrap into at most maxLines lines; returns the height used.
  int wrap(const AtFont* f, int x, int y, int w, const char* s, uint16_t c, int maxLines, int lineH = 0);
  static int wrapLines(const AtFont* f, int w, const char* s, int maxLines);

 private:
  void glyphAt(const AtGlyph* g, const AtFont* f, int x, int y, uint16_t c);
  int drawRun(const AtFont* f, int x, int y, const char* s, const char* end, uint16_t c);
};
