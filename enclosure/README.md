# AgentTally stand

A portrait case and desk stand for the 2.8" Cheap Yellow Display (ESP32-2432S028R). The screen leans back 15°, and the USB plug and cable hide inside the foot.

![Assembly](img/assembly.png) ![Back](img/assembly-back.png)

![Section through the cable path](img/section.png)

## Status

- **2.8" (`cyd28`): printed and in daily use.** Black body, white stand with inlaid letters, PLA, Bambu Lab A1 mini. Fitted to the board with two USB ports (micro-USB + USB-C). The micro-USB-only board has the same outline and mounting holes, but that fit has not been checked.
- **3.5" (`cyd35`): not printed.** The dimensions are estimates. The STLs are in `stl/` for anyone who wants to try, expect to adjust them.
- **Light pipe: experimental and optional.** It was printed in translucent PETG and works, but the light is too dim to be useful. The firmware alerts on the screen instead. See [Status light](#status-light-experimental).

If you print it, please open an issue with how it fit.

## Parts

![The printed parts as they lie on the bed](img/parts.png)

| File | Print orientation | Notes |
|---|---|---|
| `stl/agenttally-cyd28-front.stl` | face down | bezel with the screen window |
| `stl/agenttally-cyd28-back.stl` | outside down | rear cover with vents |
| `stl/agenttally-cyd28-stand.stl` | upright | at least 30% infill: the weight keeps it from tipping when you press the screen |
| `stl/agenttally-cyd28-stand_text.stl` | with the stand | optional. The letters as a separate body, for a second colour. Load it as a part of the stand object so it stays in place. Without it the name is a 0.6 mm engraving |
| `stl/agenttally-cyd28-plate.stl` | flat | closes the cable tunnel, recesses for 8 mm rubber feet |
| `stl/agenttally-cyd28-light_pipe.stl` | on its side, as exported | optional, see below |

The STLs are already in print orientation. No supports. `light_pipe_beside.stl` is the same light pipe placed next to the stand and is only a helper for slicer projects.

**Hardware:** 4× M3×14 countersunk screws (they cut their own thread in the bezel posts, longer ones bottom out), a USB-A to USB-C or USB-A to micro-USB data cable, optional 4× 8 mm rubber feet.

## Print settings

These are the settings of the Bambu Studio project that `bambu/make_3mf.py` builds. Times and weights are Bambu Studio 2.9.1 estimates for an A1 mini with a 0.4 mm nozzle, not measurements.

| Plate | Parts | Settings | Time | Filament |
|---|---|---|---|---|
| 1 Body | bezel, rear cover, bottom plate, light pipe | 3 walls, 15% infill | 1 h 03 min | 34 g |
| 2 Stand, 2 colours | stand with inlaid letters (needs an AMS) | 4 walls, 40% gyroid | 3 h 42 min | 86 g (83 g stand colour, 3 g letters, including the prime tower and purge) |
| 3 Stand, 1 colour | stand, name engraved | 4 walls, 40% gyroid | 2 h 40 min | 69 g |

All plates: PLA, 0.2 mm layers, no supports, stock presets (`0.20mm Standard @BBL A1M`, `Bambu PLA Basic @BBL A1M`). Print plate 1 and either plate 2 or plate 3. Only PLA has been tested.

```sh
python3 bambu/make_3mf.py   # needs Bambu Studio, writes print/agenttally-a1mini.3mf and print/agenttally-lightpipe.3mf
```

The `print/` folder is not in the repository. The script arranges and slices the plates with the Bambu Studio command line, using only system presets. Walls and infill are set per object. `agenttally-lightpipe.3mf` holds two light pipes in translucent PETG (0.12 mm layers, 100% infill, 20 min, 1.3 g).

## Status light (experimental)

The board's RGB LED sits on the back, so you can't see it from the front. The `light_pipe` part carries its light to a 12 mm light bar on the right edge of the bezel. In practice the bar glows too weakly to notice in daylight, so treat it as an experiment. Without it the LED still shines through the vents onto the wall behind.

![Light pipe section](img/light-section.png)

1. A 45° face right behind the LED reflects its light sideways (total internal reflection, no paint needed).
2. The bar runs behind the PCB to the right edge, where a second 45° face turns it forward.
3. It climbs a groove inside the right wall, past the PCB and the display, and ends flush in a slot in the bezel.

The firmware drives the LED for every state: **red breathing** = needs you, **steady green** = done, **slow amber** = working, off = idle.

The LED position (`led_xy`) was measured from a photo of one dual-USB board (±1.5 mm), the bar is 5 mm wide to tolerate that. Coloured transparent filament tints the light: orange PETG passes red well but makes green look yellow.

Material: translucent PETG at 0.12 mm layers and 100% infill is what was tested. White PLA is untested.

**If you skip it:** the default bezel (`light_pipe = true`) still has a slot of about 12 × 1.2 mm on its right edge. You can leave it open, nothing depends on it. You can also print the pipe in the body colour as a plain filler, which is what plate 1 of the Bambu project does (this PLA version has not been printed). For a bezel without the slot, set `light_pipe = false` in `agenttally.scad` and export again (also not printed).

## Assembly

![Exploded view, screen side down: bezel, board, light pipe (optional), rear cover](img/exploded.png)

1. Lay the board face down in the front bezel. The USB port goes to the bottom, the end with the wider bezel.
2. Optional, light pipe: slide its thin leg down the groove in the right wall until its shoulder stops and the tip sits flush in the bezel slot. Its long arm then lies across the back of the board, with the angled end over the RGB LED. Two pins in the rear cover hold it down.
3. Put the rear cover on and drive the four M3 screws through it and the board into the bezel posts. Stop when they are snug, the posts are plastic.
4. Push the cable up through the foot, from the bottom opening into the socket, and plug it into the board.
5. Seat the body in the socket. Press the cable into the tunnel along the bottom of the foot so it leaves through the notch at the back.
6. Press the plate into the recess under the foot and add rubber feet.

## Customising

Open `agenttally.scad` in OpenSCAD (2024 or newer, or MakerWorld's Parametric Model Maker). Useful parameters:

| Parameter | Default | |
|---|---|---|
| `board` | `cyd28` | `cyd35` is a 3.5" preset with **estimated** dimensions, not printed |
| `label` | `agent tally` | the text on the front of the stand |
| `light_pipe` | `true` | `false` removes the slot in the bezel and the pins in the rear cover |
| `tilt` | 15 | screen lean in degrees |
| `plug_room` | 34 | space below the board for the plug and its bend. About 16 with a 90° USB cable or adapter, which makes the whole stand much lower |
| `foot_depth` | 80 | footprint depth |
| `clear`, `fit` | 0.4, 0.25 | clearances around the PCB and between the body and the socket |
| `touch_gap` | 0.3 | gap between the bezel and the resistive touch film. Pressing on the film edge causes phantom touches |

`render.sh` exports all STLs and the preview images. It uses `openscad` from your PATH, `$OPENSCAD`, or a Docker image.

Licensed [CC BY 4.0](LICENSE).
