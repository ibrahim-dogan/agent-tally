# Hardware notes

Facts about the Sunton "Cheap Yellow Display" family collected while building AgentTally. Sources are linked inline. Measurements marked *estimate* could not be confirmed from a primary source. Corrections are very welcome.

Source shorthand:
- **RZ**: [rzeldent/platformio-espressif32-sunton](https://github.com/rzeldent/platformio-espressif32-sunton) board JSONs
- **WM**: [witnessmenow/ESP32-Cheap-Yellow-Display](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display)
- **LGFX**: [LovyanGFX autodetect](https://github.com/lovyan03/LovyanGFX/blob/master/src/lgfx/v1_autodetect/LGFX_AutoDetect_ESP32_all.hpp)

## Common to all boards

| Function | Pins |
|---|---|
| Display (HSPI) | MOSI 13, MISO 12, SCLK 14, CS 15, DC 2, RST tied to EN |
| RGB LED (active low) | R 4, G 16, B 17 |
| LDR | 34 |
| Speaker (DAC2 → FM8002A amp, 2-pin connector) | 26 |
| SD card (VSPI) | CS 5, MOSI 23, SCLK 18, MISO 19 |
| BOOT button | 0 |

The USB-serial chip is a CH340 (VID:PID `1a86:7523`) on the boards we checked.

## Per board

| | 2432S028R (2.8") | 3248S035R/C (3.5") | 2432S032R/C (3.2") | 2432S024R/C (2.4") |
|---|---|---|---|---|
| Panel | ILI9341, or ST7789 on most dual-USB boards | ST7796 320×480 | ST7789 (IPS) | ILI9341 |
| Backlight | 21 | 27 | 27 | 27 |
| Resistive touch (R) | XPT2046 on its own pins: CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36 | XPT2046 on the display bus, CS 33, IRQ 36 | same as 3.5" | same as 3.5" |
| Capacitive touch (C) | – | GT911, SDA 33, SCL 32, INT 21, RST 25, 0x5D | GT911 | CST820, 0x15 |

## The 2.8" dual-USB board ("CYD2USB")

What we measured on our board (`hello` says `panel st7789`):

- Reading `RDDID` (0x04) over MISO returns `0x81` in the first byte, not the documented `0x85` for ST7789 and not `0x00` for ILI9341. The firmware therefore falls back to ST7789 for any unknown ID. `RDDST` (0x09) returned `0x00610000`.
- With LovyanGFX's stock ST7789 init the dark greys look blue and green looks lime. Selecting gamma curve 2 (`GAMSET 0x26 = 0x02`) gives neutral greys. [WM's cyd.md](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/cyd.md) describes a similar gamma workaround.
- It is a TN panel: colours shift strongly with the viewing angle.
- Its USB-C port has no CC resistors, so use a USB-A to USB-C cable ([cyd.md](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/cyd.md)).
- Pushing frames with DMA and then reusing the same buffer straight away tears the picture on hardware (the simulator hides it). The firmware double-buffers its 40-row bands.

Some USB-C-only boards use an ILI9341 with a custom init instead (RZ `esp32-2432S028Rv2`). `agenttally.py device panel=1` forces ILI9341 if auto-detection picks the wrong driver.

## Dimensions (2.8" boards)

| Item | Value | Source |
|---|---|---|
| PCB | 86.0 × 50.0 mm, 1.6 mm thick | [Sunton drawing](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/OriginalDocumentation/3-Structure_Diagram/Dimensions.png); thickness from existing enclosures |
| Mounting holes | Ø3.2, centres 4.0 mm from each edge (78 × 42 spacing) | Sunton drawing |
| Display module | 50.0 × 69.2 × 2.3 mm (+0.3 mm foam) | [JC2432A028N datasheet](https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display/blob/main/OriginalDocumentation/4-Driver_IC_Data_Sheet/JC2432A028N.pdf) |
| Active area | 43.2 × 57.6 mm, 3.4 mm from the long sides, 3.05 mm from the end away from the FPC | datasheet |
| Glass above the PCB front | 4.0–4.5 mm | existing enclosures (brianblong, deckhand, likeablob) |
| Tallest parts on the back | ~6 mm (edge connectors) | existing enclosures |
| USB | Short edge, roughly centred; the wide (FPC) bezel side faces the USB end | drawing + photos |
| SD slot | Left long edge (seen from the front, USB down), 31–44 mm from the USB end, sticks out ~1 mm | drawing, *estimate* |
| LDR | Front, top-left corner (USB down) | drawing + photos |
| RGB LED | Back side, 11.6 mm from the left edge and 54.8 mm from the USB edge (front view, USB down), right next to the CN1 connector | photo with a ruler, ±1.5 mm |

The 3.5" preset in the enclosure (`board = "cyd35"`) uses 101.5 × 54.9 mm from [espboards](https://www.espboards.dev/esp32/cyd-esp32-3248s035/). Its holes, display offset and stack height are **estimates**, please measure before printing.
