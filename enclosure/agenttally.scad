// AgentTally - portrait desk stand for the ESP32 "Cheap Yellow Display"
// https://github.com/ibrahim-dogan/agent-tally            License: CC BY 4.0
//
// Four printed parts:
//   front  - bezel with screen window (print face down)
//   back   - rear cover with vents, M3 screw bosses (print outside down)
//   stand  - tilted socket + hollow foot, hides the USB plug and cable
//   plate  - bottom plate, closes the cable tunnel, holds rubber feet
//
// Hardware: 4x M3x14 countersunk screws (self-tap into the bezel posts),
// optional 4x rubber feet (8 mm). Print the stand with >= 30% infill: its
// weight keeps the display from tipping when you press the resistive screen.
//
// Board coordinates: x across the board (0..pcb_w), y along it with y = 0 at
// the USB edge (portrait, USB at the bottom), z = 0 on the PCB front face,
// display towards +z, components on the back towards -z.

/* [Render] */
part = "assembly"; // [assembly, exploded, body, section, light_section, front, back, stand, stand_text, plate, light_pipe, light_pipe_beside, print_all]
board = "cyd28";   // [cyd28, cyd35]
show_board = true;

/* [Stand] */
tilt = 15;          // degrees the screen leans back
plug_room = 34;     // straight room below the board for the USB plug + bend (use ~16 with a 90-degree cable)
foot_depth = 80;    // front-to-back footprint of the foot
socket_depth = 12;  // how deep the body sits in the socket
label = "agent tally";  // text on the front of the stand

/* [Status light] */
// Light pipe that carries the RGB LED on the back of the board to a light bar
// on the right edge of the bezel. Experimental: printed in translucent PETG it is dim.
light_pipe = true;
led_xy = [11.6, 54.8];  // LED centre in board coordinates (front view, USB down), measured on a dual-USB board
led_h = 1.6;            // LED height above the PCB back
bar_len = 12;           // visible light bar length on the bezel

/* [Fit] */
clear = 0.4;        // gap around the PCB inside the shell
fit = 0.25;         // body <-> socket clearance
wall = 2.0;
front_t = 1.8;      // bezel plate thickness
back_t = 1.8;
touch_gap = 0.3;    // keep the bezel off the resistive touch film

/* [Hidden] */
$fn = 48;
eps = 0.01;

// ---------------------------------------------------------------- board presets
// pcb_w, pcb_h, pcb_t, hole inset, hole d, glass height above PCB front,
// back clearance, active area [x0, y0, w, h], usb slot [center x, width],
// sd slot on the left edge [y0, y1] (or [] for none)
function preset(name) =
  name == "cyd35" ?
    [55, 101.5, 1.6, 4.0, 3.2, 4.6, 7.5, [2.5, 17, 48.96, 73.44], [27.5, 30], [40, 55]] :
  // cyd28: ESP32-2432S028R, micro-USB and dual-USB versions (Sunton drawing + JC2432A028N datasheet)
    [50, 86, 1.6, 4.0, 3.2, 4.2, 7.0, [3.4, 16.95, 43.2, 57.6], [25, 30], [31, 44]];

P = preset(board);
pcb_w = P[0]; pcb_h = P[1]; pcb_t = P[2];
hole_in = P[3]; hole_d = P[4];
glass_z = P[5];
back_clear = P[6];
aa = P[7];
usb = P[8];
sd = P[9];

// derived shell geometry
in_x0 = -clear; in_x1 = pcb_w + clear;
in_y0 = -clear; in_y1 = pcb_h + clear;
out_x0 = in_x0 - wall; out_x1 = in_x1 + wall;
out_y0 = in_y0 - wall; out_y1 = in_y1 + wall;
body_w = out_x1 - out_x0;
body_h = out_y1 - out_y0;
z_front_in = glass_z + touch_gap;          // inner face of the bezel plate
z_front = z_front_in + front_t;            // outer face (screen side)
z_split = -pcb_t;                          // front/back parting plane
z_back_in = -pcb_t - back_clear;
z_back = z_back_in - back_t;               // outer face of the rear cover
body_d = z_front - z_back;
corner_r = 4;

holes = [[hole_in, hole_in], [pcb_w - hole_in, hole_in],
         [hole_in, pcb_h - hole_in], [pcb_w - hole_in, pcb_h - hole_in]];

// ---------------------------------------------------------------- helpers
module rrect(x0, y0, x1, y1, r, h, z = 0) {
  translate([0, 0, z]) linear_extrude(h)
    hull() for (x = [x0 + r, x1 - r], y = [y0 + r, y1 - r]) translate([x, y]) circle(r);
}

module shell_outline(z0, z1) { rrect(out_x0, out_y0, out_x1, out_y1, corner_r, z1 - z0, z0); }
module cavity(z0, z1) { rrect(in_x0, in_y0, in_x1, in_y1, max(0.5, corner_r - wall), z1 - z0, z0); }

// USB opening through the bottom wall (both ports on the 2-USB board)
module usb_cut() {
  translate([usb[0] - usb[1] / 2, out_y0 - 1, z_back_in + 0.5])
    cube([usb[1], wall + clear + 2 + 3, -z_back_in + 0.5]);
}

module sd_cut() {
  if (len(sd) == 2)
    translate([out_x0 - 1, sd[0], -pcb_t - 3.2]) cube([wall + clear + 1.5, sd[1] - sd[0], 4.4]);
}

// ---------------------------------------------------------------- status light pipe
// A clear bar with two 45-degree total-internal-reflection faces:
//   LED (facing backwards) -> mirror 1 turns the light sideways along the board ->
//   mirror 2 at the right edge turns it forward -> up a groove in the side wall,
//   past the PCB and the display -> out through a slot in the bezel.
has_lp = light_pipe && board == "cyd28";
lp_t = 2.0;                              // bar thickness (z) = width of the turn
lp_w = 5.0;                              // bar width (y), generous for LED position error
lp_z0 = -pcb_t - led_h - 0.6;            // face towards the PCB, just behind the LED
lp_z1 = lp_z0 - lp_t;
lp_x1 = in_x1 + wall - 0.6;              // outer face of the vertical leg (0.6 mm wall skin left)
lp_x0 = lp_x1 - lp_t;                    // inner face at the turn (behind the PCB)
lp_xe = pcb_w + 0.1;                     // inner face of the leg beside the PCB / display
lp_tip = 0.25;                           // shoulder on each side of the tip that stops the pipe flush
ly0 = led_xy[1] - lp_w / 2;
ly1 = ly0 + bar_len;

module light_pipe_body() {
  difference() {
    union() {
      // horizontal run behind the PCB
      translate([led_xy[0] - lp_t / 2, ly0, lp_z1]) cube([lp_x1 - led_xy[0] + lp_t / 2, lp_w, lp_t]);
      // the turn, as wide as the bar is thick
      translate([lp_x0, ly0, lp_z1]) cube([lp_t, lp_w, lp_t]);
      // vertical leg, fanning out to the bar length
      hull() {
        translate([lp_xe, ly0, lp_z0 - eps]) cube([lp_x1 - lp_xe, lp_w, eps]);
        translate([lp_xe, ly0, z_front_in - eps]) cube([lp_x1 - lp_xe, bar_len, eps]);
      }
      // tip through the bezel plate, flush with the front
      translate([lp_xe + lp_tip, ly0, z_front_in - eps]) cube([lp_x1 - lp_xe - 2 * lp_tip, bar_len, front_t + eps]);
    }
    // mirror 1 over the LED: remove the half-space x + z < c (far-left corner)
    xz_cut(led_xy[0] - lp_t / 2, lp_z0, [[-30, 30], [30, -30], [-30, -30]]);
    // mirror 2 at the edge: remove the half-space x - z > c (far-right corner)
    xz_cut(lp_x1, lp_z0, [[-30, -30], [30, 30], [30, -30]]);
  }
}

// extrude a polygon given in (x, z) along y, placed at (x0, z0)
module xz_cut(x0, z0, pts) {
  translate([x0, 0, z0]) rotate([90, 0, 0]) linear_extrude(400, center = true) polygon(pts);
}

// clearance cut in the shell for the pipe (groove in the right wall + slot in the bezel)
module light_pipe_cut() {
  c = 0.15;
  // horizontal pocket so the back cover lip/bosses never touch the bar
  translate([lp_x0 - c, ly0 - c, lp_z1 - c]) cube([lp_x1 - lp_x0 + 2 * c, lp_w + 2 * c, lp_z0 - lp_z1 + 2 * c]);
  hull() {
    translate([lp_xe - c, ly0 - c, lp_z1 - c]) cube([lp_x1 - lp_xe + 2 * c, lp_w + 2 * c, eps]);
    translate([lp_xe - c, ly0 - c, lp_z0 - c]) cube([lp_x1 - lp_xe + 2 * c, lp_w + 2 * c, eps]);
    translate([lp_xe - c, ly0 - c, z_front_in - eps]) cube([lp_x1 - lp_xe + 2 * c, bar_len + 2 * c, eps]);
  }
  translate([lp_xe + lp_tip - c, ly0 - c, z_front_in - 1]) cube([lp_x1 - lp_xe - 2 * lp_tip + 2 * c, bar_len + 2 * c, front_t + 2]);
}

// two fingers on the back cover hold the bar against the turn
module light_pipe_fingers() {
  // between the vent slots (vents at 9 + i * (pcb_w - 18) / 6)
  step = (pcb_w - 18) / 6;
  for (x = [9 + 2.5 * step, 9 + 5.5 * step])
    translate([x, led_xy[1], z_back_in - eps]) cylinder(d = 3, h = lp_z1 - z_back_in + eps);
}

// ---------------------------------------------------------------- front bezel
module front() {
  win = [aa[0] - 0.6, aa[1] - 0.6, aa[2] + 1.2, aa[3] + 1.2];
  difference() {
    union() {
      difference() {
        shell_outline(z_split, z_front);
        cavity(z_split - eps, z_front_in);
      }
      // posts that land on the PCB around the mounting holes
      for (h = holes) translate([h[0], h[1], 0]) cylinder(d = 5.6, h = z_front_in + eps);
      // alignment lip that slides into the rear cover
    }
    // screen window with a 45 degree chamfer towards the viewer
    hull() {
      translate([win[0], win[1], z_front_in - eps]) cube([win[2], win[3], eps]);
      translate([win[0] - front_t, win[1] - front_t, z_front + eps]) cube([win[2] + 2 * front_t, win[3] + 2 * front_t, eps]);
    }
    translate([win[0], win[1], z_front_in - 1]) cube([win[2], win[3], 2]);
    // blind pilot holes for M3 thread-forming screws
    for (h = holes) translate([h[0], h[1], -eps]) cylinder(d = 2.6, h = z_front - 0.8);
    usb_cut();
    sd_cut();
    if (has_lp) light_pipe_cut();
    // lip recess for the rear cover
    difference() {
      rrect(in_x0 - 0.9, in_y0 - 0.9, in_x1 + 0.9, in_y1 + 0.9, corner_r - wall + 0.9, 1.6, z_split - eps);
      rrect(in_x0 - eps, in_y0 - eps, in_x1 + eps, in_y1 + eps, corner_r - wall, 4, z_split - 1);
    }
  }
}

// ---------------------------------------------------------------- rear cover
module back() {
  difference() {
    union() {
      difference() {
        shell_outline(z_back, z_split);
        cavity(z_back_in, z_split + eps);
      }
      // lip that keys into the bezel recess
      difference() {
        rrect(in_x0 - 0.75, in_y0 - 0.75, in_x1 + 0.75, in_y1 + 0.75, corner_r - wall + 0.75, 1.4, z_split - eps);
        rrect(in_x0 + eps, in_y0 + eps, in_x1 - eps, in_y1 - eps, corner_r - wall, 4, z_split - 1);  // taller than the lip: keep the tray open
      }
      // screw bosses up to the PCB back
      for (h = holes) translate([h[0], h[1], z_back_in - eps]) cylinder(d = 5.6, h = back_clear + eps);
      if (has_lp) light_pipe_fingers();
    }
    for (h = holes) {
      translate([h[0], h[1], z_back - eps]) cylinder(d = 3.4, h = body_d);
      // countersink for M3 flat heads
      translate([h[0], h[1], z_back - eps]) cylinder(d1 = 6.4, d2 = 3.4, h = 1.6);
    }
    usb_cut();
    sd_cut();
    vents();
    if (has_lp) light_pipe_cut();
  }
}

// vertical slots in the upper half: airflow, and they let the RGB LED glow through
module vents() {
  n = 7;
  span = pcb_w - 18;
  for (i = [0 : n - 1])
    translate([9 + i * span / (n - 1) - 0.9, pcb_h * 0.52, z_back - 1])
      hull() {
        translate([0.9, 0, 0]) cylinder(d = 1.8, h = back_t + 2, $fn = 16);
        translate([0.9, pcb_h * 0.3, 0]) cylinder(d = 1.8, h = back_t + 2, $fn = 16);
      }
}

// ---------------------------------------------------------------- stand
// The body is placed in "stand space" (X across, Y towards the back, Z up):
// stood upright with the screen facing -Y, leaned back by `tilt` and lifted by
// plug_room so the USB plug hangs into the hollow foot.
module place_body() {
  translate([0, 0, plug_room]) rotate([-tilt, 0, 0]) rotate([90, 0, 0])
    translate([-(out_x0 + out_x1) / 2, -out_y0, -(z_front + z_back) / 2]) children();
}

// Body silhouette (for the socket cut), extended downwards.
module body_block(extra = 0, below = 0) {
  rrect(out_x0 - extra, out_y0 - below, out_x1 + extra, out_y1, corner_r + extra, body_d + 2 * extra, z_back - extra);
}

stand_w = body_w + 2 * 2.2;                 // flush with the socket walls
foot_w = stand_w + 6;                       // slight flare at the desk
// where the screen plane, continued downwards, meets the desk
foot_front = -body_d / 2 * cos(tilt) - plug_room * tan(tilt) - 2.2;
foot_back = foot_front + foot_depth;
cable_h = 9;   // tunnel height for the cable
// y where the plug shaft meets the desk (back face of the shaft)
function shaft_y0() = -body_d / 2 * cos(tilt) - plug_room * tan(tilt) + 2;

module stand_outer() {
  hull() {
    translate([-foot_w / 2, foot_front, 0]) rrect(0, 0, foot_w, foot_depth, 8, 1);
    place_body() intersection() {
      body_block(extra = 2.2);
      translate([out_x0 - 10, out_y0 - 10, z_back - 10]) cube([body_w + 20, socket_depth + 10, body_d + 20]);
    }
  }
}

module stand() {
  difference() {
    stand_outer();
    // socket for the body
    place_body() body_block(extra = fit);
    // keep the front lip low so the bezel stays visible
    place_body() translate([out_x0 - 5, out_y0 + 3, z_front - 0.4]) cube([body_w + 10, socket_depth + 10, 10]);
    // plug shaft along the body axis, from the socket floor down to the desk
    intersection() {
      place_body() translate([usb[0] - usb[1] / 2 - 2, out_y0 - plug_room * 2, z_back + 1.6])
        cube([usb[1] + 4, plug_room * 2 + eps, body_d - 3.2]);
      translate([-100, -100, -eps]) cube([200, 300, 300]);
    }
    // cable tunnel to the rear exit, open at the bottom (12 mm bridge when printing)
    hull() {
      translate([-6, shaft_y0(), -eps]) cube([12, 10, cable_h]);
      translate([-6, foot_back - 14, -eps]) cube([12, 1, cable_h]);
    }
    // round the turn from the shaft into the tunnel
    hull() {
      translate([-6, shaft_y0(), -eps]) cube([12, 10, cable_h]);
      intersection() {
        place_body() translate([usb[0] - 6, out_y0 - plug_room * 2, z_back + 1.6]) cube([12, plug_room * 2, body_d - 3.2]);
        translate([-100, -100, -eps]) cube([200, 300, cable_h + 8]);
      }
    }
    // plate recess
    translate([-foot_w / 2 + 2, foot_front + 2, -eps]) rrect(0, 0, foot_w - 4, foot_depth - 4, 6, 2.2);
    // cable exit at the rear, open to the bottom so the cable drops in
    translate([-4, foot_back - 15, -eps]) cube([8, 17, 6]);
    translate([0, foot_back - 15, 6]) rotate([-90, 0, 0]) cylinder(d = 8, h = 17);
    // engraved name on the sloped front face (printed as a colour inlay with AMS)
    stand_text_volume();
  }
}

module stand_text_volume() {
  translate([0, 0, plug_room * 0.45]) rotate([90 - tilt, 0, 0])
    translate([0, 0, -foot_front - plug_room * 0.45 * tan(tilt) - 0.6])
      linear_extrude(2) text(label, size = 4.2, font = "Liberation Sans:style=Bold", halign = "center", valign = "center");
}

// the letters only, to print in a second colour inside the engraving
module stand_text() {
  intersection() {
    stand_outer();
    stand_text_volume();
  }
}

// ---------------------------------------------------------------- bottom plate
module plate() {
  pw = foot_w - 4 - 0.4;
  pd = foot_depth - 4 - 0.4;
  difference() {
    rrect(0, 0, pw, pd, 5.8, 2);
    // rubber feet recesses
    for (x = [9, pw - 9], y = [9, pd - 9]) translate([x, y, -eps]) cylinder(d = 8.6, h = 0.8);
    // notch where the cable leaves the tunnel
    translate([pw / 2 - 4.2, pd - 14, -eps]) cube([8.4, 15, 3]);
  }
}

// ---------------------------------------------------------------- board model (preview only)
module board_model() {
  color("gold") difference() {
    rrect(0, 0, pcb_w, pcb_h, 1.5, pcb_t, -pcb_t);
    for (h = holes) translate([h[0], h[1], -5]) cylinder(d = hole_d, h = 10);
  }
  color([0.15, 0.15, 0.18]) translate([0, 8.4, 0]) cube([pcb_w, 69.2, glass_z]);
  color([0.25, 0.35, 0.55]) translate([aa[0], aa[1], glass_z - 0.05]) cube([aa[2], aa[3], 0.1]);
  color("silver") translate([usb[0] - 4.5, -0.6, -pcb_t - 3.4]) cube([9, 7.5, 3.4]);
  color([0.2, 0.2, 0.2]) translate([16, 60, -pcb_t - 3.2]) cube([18, 20, 3.2]);  // ESP32 module
  color("red") translate([led_xy[0], led_xy[1], -pcb_t - led_h / 2]) cube([2.5, 2.5, led_h], center = true);
}

// ---------------------------------------------------------------- output
// preview colours: the tested print is a black body on a white stand with dark letters
c_body = [0.17, 0.17, 0.19];
c_stand = [0.95, 0.95, 0.93];

module body_assembly(explode = 0) {
  color(c_body) translate([0, 0, explode]) front();
  color(c_body * 1.25) translate([0, 0, -explode]) back();
  if (has_lp) color([1, 0.45, 0.1, 0.85]) light_pipe_body();
  if (show_board) board_model();
}


if (part == "front") rotate([180, 0, 0]) translate([0, 0, -z_front]) front();
else if (part == "back") translate([0, 0, -z_back]) back();
else if (part == "stand") stand();
else if (part == "plate") plate();
else if (part == "body") body_assembly();
// assembly order, screen facing down: bezel, board, light pipe, rear cover
else if (part == "exploded") rotate([180, 0, 0]) {
  color([0.25, 0.25, 0.27]) translate([0, 0, 0]) front();
  translate([0, 0, -22]) board_model();
  if (has_lp) color([1, 0.45, 0.1, 0.9]) translate([0, 0, -38]) light_pipe_body();
  color([0.35, 0.35, 0.38]) translate([0, 0, -60]) back();
}
else if (part == "stand_text") stand_text();
// a light pipe parked next to the stand, so both print as one multi-part object
else if (part == "light_pipe_beside") translate([foot_w / 2 + 8, foot_front, 0]) rotate([0, 0, 90]) rotate([90, 0, 0]) translate([-led_xy[0], -ly0, 0]) light_pipe_body();
// printed lying on its side (y = ly0 face down): the fan rises from the bed, no supports
else if (part == "light_pipe") rotate([90, 0, 0]) translate([0, -ly0, 0]) light_pipe_body();
else if (part == "light_section") {
  // cut across the board at the LED height, seen from the USB end
  intersection() {
    body_assembly();
    translate([-20, led_xy[1] - 0.5, -50]) cube([100, 100, 100]);
  }
}
else if (part == "print_all") {
  // every part as it lies on the bed (preview image only, the STLs are exported one by one)
  color(c_body * 1.6) translate([0, out_y0 + out_y1, 0]) rotate([180, 0, 0]) translate([0, 0, -z_front]) front();
  color(c_body * 1.9) translate([body_w + 8, 0, -z_back]) back();
  translate([-foot_w / 2 - 10, -foot_front, 0]) {
    color(c_stand) stand();
    color(c_body) render() stand_text();
  }
  color(c_body * 1.6) translate([-foot_w * 2 - 30, 0, 0]) plate();
  if (has_lp) color([1, 0.45, 0.1, 0.9]) translate([2 * body_w + 14, 20, 0]) rotate([90, 0, 0]) translate([0, -ly0, 0]) light_pipe_body();
} else if (part == "section") {
  // cut through the USB slot to check the cable path
  intersection() {
    union() {
      color(c_stand) stand();
      place_body() body_assembly();
    }
    translate([-100, -100, -1]) cube([100 + usb[0] - (out_x0 + out_x1) / 2, 300, 300]);
  }
} else {
  color(c_stand) stand();
  color(c_body) render() stand_text();
  place_body() body_assembly();
  color([0.6, 0.6, 0.62]) translate([-(foot_w - 4 - 0.4) / 2, foot_front + 2.2, 0]) plate();
}
