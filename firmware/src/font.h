#pragma once
#include <stdint.h>

// 4-bit anti-aliased bitmap font produced by tools/mkfont.py.
struct AtGlyph {
  uint16_t cp;       // unicode code point (sorted ascending)
  uint8_t w, h;      // bitmap size
  int8_t x, y;       // bitmap offset from pen x / line top
  uint8_t adv;       // advance width
  uint32_t off;      // byte offset into bits (rows packed, 2 px per byte, high nibble first)
};

struct AtFont {
  const uint8_t* bits;
  const AtGlyph* glyphs;
  uint16_t count;
  uint8_t height;    // line height (ascent + descent)
  uint8_t ascent;
};
