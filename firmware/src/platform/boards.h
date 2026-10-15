#pragma once
// Board definitions for the Sunton "Cheap Yellow Display" family (ESP32, not S3).
//
// cyd28 uses LovyanGFX's own Sunton 2432S028 autodetect, which reads the panel
// ID over MISO and picks ILI9341 (original micro-USB board) or ST7789 (most
// dual-USB "CYD2USB" boards). The other boards are configured by hand from the
// rzeldent/platformio-espressif32-sunton definitions; see docs/HARDWARE.md.
//
// Common to all: display on HSPI (MOSI 13, MISO 12, SCLK 14, CS 15, DC 2),
// RGB LED 4/16/17 active low, LDR 34, speaker 26 (DAC + amp).

#define LGFX_USE_V1

#if defined(AT_BOARD_CYD28)
#include <LovyanGFX.hpp>
#define AT_BOARD_NAME "cyd28"
#define AT_PANEL_SELECT 1

// ESP32-2432S028R. The original micro-USB board has an ILI9341, most dual-USB
// (micro + USB-C) boards an ST7789. We read the panel ID (RDDID 0x04) over
// MISO; some boards leave MISO unconnected, so anything unrecognised falls
// back to ST7789. `agenttally.py device panel=1|2` forces ILI9341 / ST7789.
class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 ili_;
  lgfx::Panel_ST7789 st_;
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;
  lgfx::Touch_XPT2046 touch_;

 public:
  uint32_t probeId = 0, probeSt = 0;
  bool isIli = false;

  LGFX() {
    auto b = bus_.config();
    b.spi_host = HSPI_HOST;
    b.spi_mode = 0;
    b.freq_write = 40000000;
    b.freq_read = 16000000;
    b.spi_3wire = false;
    b.use_lock = true;
    b.dma_channel = SPI_DMA_CH_AUTO;
    b.pin_sclk = 14;
    b.pin_mosi = 13;
    b.pin_miso = 12;
    b.pin_dc = 2;
    bus_.config(b);

    auto l = light_.config();
    l.pin_bl = 21;
    l.invert = false;
    l.freq = 12000;
    l.pwm_channel = 7;
    light_.config(l);

    auto t = touch_.config();
    t.x_min = 300;
    t.x_max = 3900;
    t.y_min = 3700;
    t.y_max = 200;
    t.pin_int = -1;
    t.bus_shared = false;
    t.spi_host = -1;  // software SPI: touch has its own pins on this board
    t.pin_sclk = 25;
    t.pin_mosi = 32;
    t.pin_miso = 39;
    t.pin_cs = 33;
    t.freq = 1000000;
    touch_.config(t);
  }

  // Read a 4-byte register over MISO the way LovyanGFX's autodetect does.
  uint32_t readId(uint8_t cmd) {
    bus_.init();
    lgfx::pinMode(15, lgfx::pin_mode_t::output);
    bus_.beginTransaction();
    lgfx::gpio_hi(15);
    bus_.writeCommand(0, 8);
    bus_.wait();
    lgfx::gpio_lo(15);
    bus_.writeCommand(cmd, 8);
    bus_.beginRead(1);
    uint32_t res = 0;
    for (int i = 0; i < 4; ++i) res |= (bus_.readData(8) & 0xFF) << (i * 8);
    bus_.endTransaction();
    lgfx::gpio_hi(15);
    return res;
  }

  // force: 0 auto, 1 ILI9341, 2 ST7789
  void select(int force) {
    probeId = readId(0x04);
    probeSt = readId(0x09);
    if (force == 1) isIli = true;
    else if (force == 2) isIli = false;
    else isIli = (probeId & 0xFF) == 0x00 && (probeSt & 0xFFFFFF) != 0 && (probeSt & 0xFFFFFF) != 0xFFFFFF;

    lgfx::Panel_Device* p = isIli ? (lgfx::Panel_Device*)&ili_ : (lgfx::Panel_Device*)&st_;
    auto c = p->config();
    c.pin_cs = 15;
    c.pin_rst = -1;
    c.pin_busy = -1;
    c.panel_width = 240;
    c.panel_height = 320;
    c.memory_width = 240;
    c.memory_height = 320;
    c.offset_rotation = isIli ? 2 : 0;
    c.readable = true;
    c.invert = false;
    c.rgb_order = false;
    c.bus_shared = false;
    p->config(c);
    p->setBus(&bus_);
    p->setLight(&light_);
    auto t = touch_.config();
    t.offset_rotation = isIli ? 0 : 2;
    touch_.config(t);
    p->setTouch(&touch_);
    setPanel(p);
  }
  const char* panelName() const { return isIli ? "ili9341" : "st7789"; }
};

#else
#include <LovyanGFX.hpp>

#if defined(AT_BOARD_CYD24R) || defined(AT_BOARD_CYD24C)
#define AT_PANEL lgfx::Panel_ILI9341
#define AT_PANEL_NAME "ili9341"
#define AT_PANEL_W 240
#define AT_PANEL_H 320
#define AT_PANEL_INVERT false
#define AT_PANEL_OFFSET_ROT 0
#define AT_SPI_FREQ 40000000
#elif defined(AT_BOARD_CYD32R) || defined(AT_BOARD_CYD32C)
#define AT_PANEL lgfx::Panel_ST7789
#define AT_PANEL_NAME "st7789"
#define AT_PANEL_W 240
#define AT_PANEL_H 320
#define AT_PANEL_INVERT true
#define AT_PANEL_OFFSET_ROT 0
#define AT_SPI_FREQ 40000000
#elif defined(AT_BOARD_CYD35R) || defined(AT_BOARD_CYD35C)
#define AT_PANEL lgfx::Panel_ST7796
#define AT_PANEL_NAME "st7796"
#define AT_PANEL_W 320
#define AT_PANEL_H 480
#define AT_PANEL_INVERT false
#define AT_PANEL_OFFSET_ROT 0
#define AT_SPI_FREQ 40000000
#else
#error "Select a board: -DAT_BOARD_CYD28 / CYD24R / CYD24C / CYD32R / CYD32C / CYD35R / CYD35C"
#endif

#if defined(AT_BOARD_CYD24R)
#define AT_BOARD_NAME "cyd24r"
#elif defined(AT_BOARD_CYD24C)
#define AT_BOARD_NAME "cyd24c"
#elif defined(AT_BOARD_CYD32R)
#define AT_BOARD_NAME "cyd32r"
#elif defined(AT_BOARD_CYD32C)
#define AT_BOARD_NAME "cyd32c"
#elif defined(AT_BOARD_CYD35R)
#define AT_BOARD_NAME "cyd35r"
#else
#define AT_BOARD_NAME "cyd35c"
#endif

class LGFX : public lgfx::LGFX_Device {
  AT_PANEL panel_;
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;
#if defined(AT_BOARD_CYD24C)
  lgfx::Touch_CST816S touch_;
#elif defined(AT_BOARD_CYD32C) || defined(AT_BOARD_CYD35C)
  lgfx::Touch_GT911 touch_;
#else
  lgfx::Touch_XPT2046 touch_;
#endif

 public:
  LGFX() {
    {
      auto cfg = bus_.config();
      cfg.spi_host = HSPI_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = AT_SPI_FREQ;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc = 2;
      bus_.config(cfg);
      panel_.setBus(&bus_);
    }
    {
      auto cfg = panel_.config();
      cfg.pin_cs = 15;
      cfg.pin_rst = -1;
      cfg.pin_busy = -1;
      cfg.panel_width = AT_PANEL_W;
      cfg.panel_height = AT_PANEL_H;
      cfg.memory_width = AT_PANEL_W;
      cfg.memory_height = AT_PANEL_H;
      cfg.offset_rotation = AT_PANEL_OFFSET_ROT;
      cfg.readable = true;
      cfg.invert = AT_PANEL_INVERT;
      cfg.rgb_order = false;
      cfg.bus_shared = true;  // XPT2046 shares the display bus on these boards
      panel_.config(cfg);
    }
    {
      auto cfg = light_.config();
      cfg.pin_bl = 27;
      cfg.invert = false;
      cfg.freq = 12000;
      cfg.pwm_channel = 7;
      light_.config(cfg);
      panel_.setLight(&light_);
    }
    {
      auto cfg = touch_.config();
#if defined(AT_BOARD_CYD24C) || defined(AT_BOARD_CYD32C) || defined(AT_BOARD_CYD35C)
      cfg.i2c_port = 0;
      cfg.pin_sda = 33;
      cfg.pin_scl = 32;
      cfg.pin_rst = 25;
      cfg.pin_int = -1;
      cfg.freq = 400000;
      cfg.x_min = 0;
      cfg.y_min = 0;
      cfg.x_max = AT_PANEL_W - 1;
      cfg.y_max = AT_PANEL_H - 1;
#if defined(AT_BOARD_CYD24C)
      cfg.i2c_addr = 0x15;
#else
      cfg.i2c_addr = 0x5D;
#endif
#else
      cfg.spi_host = HSPI_HOST;
      cfg.freq = 1000000;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_cs = 33;
      cfg.pin_int = 36;
      cfg.bus_shared = true;
      cfg.x_min = 300;
      cfg.x_max = 3900;
      cfg.y_min = 3700;
      cfg.y_max = 200;
#endif
      cfg.offset_rotation = 0;
      touch_.config(cfg);
      panel_.setTouch(&touch_);
    }
    setPanel(&panel_);
  }
};
#endif
