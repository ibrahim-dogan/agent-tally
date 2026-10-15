#include "gfx.h"
#include <string.h>

uint32_t Painter::nextCp(const char*& s) {
  const uint8_t* p = (const uint8_t*)s;
  uint32_t c = *p;
  if (c < 0x80) { s += 1; return c; }
  if ((c & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { s += 2; return ((c & 0x1F) << 6) | (p[1] & 0x3F); }
  if ((c & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
    s += 3;
    return ((c & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
  }
  if ((c & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { s += 4; return '?'; }
  s += 1;
  return '?';
}

const AtGlyph* Painter::glyph(const AtFont* f, uint32_t cp) {
  int lo = 0, hi = f->count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;
    uint16_t v = f->glyphs[mid].cp;
    if (v == cp) return &f->glyphs[mid];
    if (v < cp) lo = mid + 1; else hi = mid - 1;
  }
  return cp == '?' ? nullptr : glyph(f, '?');
}

int Painter::width(const AtFont* f, const char* s, int n) {
  int w = 0;
  const char* end = n < 0 ? s + strlen(s) : s + n;
  while (s < end && *s) {
    const AtGlyph* g = glyph(f, nextCp(s));
    if (g) w += g->adv;
  }
  return w;
}

void Painter::glyphAt(const AtGlyph* g, const AtFont* f, int x, int y, uint16_t c) {
  int gy = y + g->y - oy;                 // band-local top of the bitmap
  if (gy + g->h <= 0 || gy >= BH || !g->w) return;
  uint16_t* buf = (uint16_t*)spr->getBuffer();
  const uint8_t* bits = f->bits + g->off;
  int gx = x + g->x;
  int idx = 0;
  for (int row = 0; row < g->h; row++) {
    int py = gy + row;
    for (int col = 0; col < g->w; col++, idx++) {
      uint8_t a = (idx & 1) ? (bits[idx >> 1] & 0x0F) : (bits[idx >> 1] >> 4);
      if (!a || py < 0 || py >= BH) continue;
      int px = gx + col;
      if (px < 0 || px >= W) continue;
      uint16_t* d = &buf[py * W + px];
      if (a == 15) {
        *d = __builtin_bswap16(c);
      } else {
        uint16_t bg = __builtin_bswap16(*d);
        *d = __builtin_bswap16(mix(bg, c, a * 17));
      }
    }
  }
}

int Painter::drawRun(const AtFont* f, int x, int y, const char* s, const char* end, uint16_t c) {
  int x0 = x;
  bool draw = vis(y, f->height + 4);
  while (s < end && *s) {
    const AtGlyph* g = glyph(f, nextCp(s));
    if (!g) continue;
    if (draw) glyphAt(g, f, x, y, c);
    x += g->adv;
  }
  return x - x0;
}

int Painter::text(const AtFont* f, int x, int y, const char* s, uint16_t c, int maxw) {
  const char* end = s + strlen(s);
  if (maxw > 0 && width(f, s) > maxw) {
    const char* ell = "\xE2\x80\xA6";  // U+2026
    int ellw = width(f, ell);
    int w = 0;
    const char* p = s;
    const char* cut = s;
    while (*p) {
      const char* q = p;
      const AtGlyph* g = glyph(f, nextCp(q));
      int gw = g ? g->adv : 0;
      if (w + gw + ellw > maxw) break;
      w += gw;
      p = q;
      cut = p;
    }
    while (cut > s && cut[-1] == ' ') cut--;
    int adv = drawRun(f, x, y, s, cut, c);
    return adv + drawRun(f, x + adv, y, ell, ell + 3, c);
  }
  return drawRun(f, x, y, s, end, c);
}

// Find how many bytes of s fit in w pixels, breaking at a space when possible.
static int fitLine(const AtFont* f, int w, const char* s, bool* more) {
  int px = 0;
  const char* p = s;
  const char* lastSpace = nullptr;
  while (*p) {
    const char* q = p;
    uint32_t cp = Painter::nextCp(q);
    const AtGlyph* g = Painter::glyph(f, cp);
    int gw = g ? g->adv : 0;
    if (px + gw > w) {
      *more = true;
      if (lastSpace && lastSpace > s) return lastSpace - s;
      return p == s ? q - s : p - s;  // always make progress
    }
    if (cp == ' ') lastSpace = p;
    px += gw;
    p = q;
  }
  *more = false;
  return p - s;
}

int Painter::wrapLines(const AtFont* f, int w, const char* s, int maxLines) {
  int lines = 0;
  while (*s && lines < maxLines) {
    while (*s == ' ') s++;
    if (!*s) break;
    bool more;
    int n = fitLine(f, w, s, &more);
    s += n;
    lines++;
  }
  return lines;
}

int Painter::wrap(const AtFont* f, int x, int y, int w, const char* s, uint16_t c, int maxLines, int lineH) {
  if (!lineH) lineH = f->height + 2;
  int lines = 0;
  while (*s && lines < maxLines) {
    while (*s == ' ') s++;
    if (!*s) break;
    bool more;
    int n = fitLine(f, w, s, &more);
    if (lines == maxLines - 1 && more) {
      text(f, x, y + lines * lineH, s, c, w);  // last allowed line: ellipsis
      lines++;
      break;
    }
    drawRun(f, x, y + lines * lineH, s, s + n, c);
    s += n;
    lines++;
  }
  return lines * lineH;
}
