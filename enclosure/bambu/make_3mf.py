#!/usr/bin/env python3
"""Build sliced Bambu Studio projects (A1 mini, 0.4 mm nozzle) from the cyd28 STLs.

    python3 enclosure/bambu/make_3mf.py [--studio /path/to/BambuStudio.app] [--bed "Textured PEI Plate"]

Writes enclosure/print/:
  agenttally-a1mini.3mf     three plates, PLA, 0.2 mm, no supports
                              1  Body: front + back + bottom plate + light pipe   filament 1 (dark)
                              2  Stand, 2 colours: letters inlaid (AMS)           filament 2 (white) + 1
                              3  Stand, 1 colour                                  filament 1
                            The light pipe on plate 1 is printed in the body colour: it only
                            plugs the slot in the bezel. Use the PETG one if you want it to glow.
  agenttally-lightpipe.3mf  two light pipes, clear PETG, 0.12 mm, 100% infill (experimental)

Print plate 1 and either plate 2 or plate 3. Filament slots are only colour hints: Bambu Studio
maps them to your AMS or spool when you send the job.

How it works, and why:
- The printer, process and filament presets are the unmodified system presets (flattened along
  "inherits", because the CLI wants complete files). Walls and infill are set per object, so
  nothing depends on a user preset that other people do not have.
- Plates come from --load-assemble-list. Parts with the same group become one object, which is
  how the letters stay registered inside the stand.
- The CLI writes a project that claims one filament and a 4x4 purge matrix whatever was loaded.
  With two filaments that makes every G-code export fail without a message, so the project is
  patched (per-filament lists, purge matrix, colours, bed type) before it is sliced.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
STL = os.path.join(HERE, "..", "stl")
OUT = os.path.join(HERE, "..", "print")
MACHINE = "Bambu Lab A1 mini 0.4 nozzle"
PLA = "Bambu PLA Basic @BBL A1M"
PETG = "Bambu PETG Translucent @BBL A1M"
DARK, WHITE, CLEAR = "#2B2B2D", "#F4F4F2", "#FF8A1E"
# Purge volumes in mm3. Dark to white needs much more than white to dark; these are close to what
# Bambu Studio's "Auto-Calc" gives for black and white PLA. Recalculate there for other colours.
PURGE_DARK_TO_WHITE, PURGE_WHITE_TO_DARK = 700, 170

BODY = {"wall_loops": "3", "sparse_infill_density": "15%"}
# heavy on purpose: the weight keeps the stand from tipping when the screen is pressed
STAND = {"wall_loops": "4", "sparse_infill_density": "40%", "sparse_infill_pattern": "gyroid"}
PIPE = {"wall_loops": "4", "sparse_infill_density": "100%", "sparse_infill_pattern": "zig-zag",
        "outer_wall_speed": "60", "inner_wall_speed": "80"}

# Object names shown in Bambu Studio
NAMES = {"front.stl": "Front bezel", "back.stl": "Rear cover", "plate.stl": "Bottom plate", "stand.stl": "Stand",
         "light_pipe.stl": "Light pipe (optional)"}

# Per-filament options that are missing from the filament presets, so the CLI leaves them at one
# entry however many filaments are loaded.
PER_FILAMENT = ("default_filament_colour", "enable_overhang_bridge_fan", "enable_pressure_advance",
                "pressure_advance", "first_x_layer_fan_speed", "first_x_layer_optional_fan_speed",
                "first_x_layer_part_fan_speed", "ironing_fan_speed", "min_temperature_when_ec",
                "overhang_threshold_participating_cooling")

sys.path.insert(0, HERE)
from flatten import index, resolve  # noqa: E402


def find_studio(arg):
    cands = [arg] if arg else []
    cands += ["/Applications/BambuStudio.app", os.path.expanduser("~/Applications/BambuStudio.app"),
              os.path.expanduser("~/MyApps/BambuStudio.app")]
    for c in cands:
        if c and os.path.exists(os.path.join(c, "Contents/MacOS/BambuStudio")):
            return c
    sys.exit("Bambu Studio not found, pass --studio /path/to/BambuStudio.app")


class Studio:
    def __init__(self, app, work):
        self.bin = os.path.join(app, "Contents/MacOS/BambuStudio")
        profiles = os.path.join(app, "Contents/Resources/profiles")
        self.idx = index(os.path.join(profiles, "BBL"))
        with open(os.path.join(profiles, "BBL.json")) as f:
            self.version = json.load(f).get("version")
        self.work = work

    def preset(self, name, id_key):
        """A system preset with its parents merged in, still named and marked as the system one."""
        flat = resolve(self.idx[name], self.idx)
        flat["from"] = "system"
        flat["version"] = self.version
        flat[id_key] = [name] if id_key == "filament_settings_id" else name
        path = os.path.join(self.work, name.replace("/", "_") + ".json")
        with open(path, "w") as f:
            json.dump(flat, f)
        return path

    def run(self, *args):
        r = subprocess.run([self.bin, *args], capture_output=True, text=True, cwd=self.work)
        if r.returncode != 0:
            errs = [l for l in (r.stdout + r.stderr).splitlines() if "[error]" in l]
            sys.exit(f"Bambu Studio failed ({r.returncode}): " + "\n".join(errs[-5:]))


def patch_project(src, dst, colours, purge, bed, settings):
    """Make the filament lists consistent, set colours and bed, add per-object settings."""
    n = len(colours)
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
        for item in zin.infolist():
            data = zin.read(item.filename)
            if item.filename == "Metadata/project_settings.config":
                cfg = json.loads(data)
                for k, v in cfg.items():
                    per_filament = k.startswith("filament_") or k in PER_FILAMENT
                    if per_filament and isinstance(v, list) and len(v) == 1 and n > 1:
                        cfg[k] = v * n
                cfg["filament_colour"] = colours
                if n > 1:
                    cfg["flush_volumes_matrix"] = [str(v) for row in purge for v in row]
                    cfg["flush_volumes_vector"] = ["140"] * (2 * n)
                cfg["curr_bed_type"] = bed
                data = json.dumps(cfg, indent=4).encode()
            elif item.filename == "Metadata/model_settings.config":
                todo = list(settings)  # objects are written in assemble-list order

                def add(m):
                    kv = dict(todo.pop(0))
                    head = f'<metadata key="name" value="{kv.pop("name")}"/>'
                    head += "".join(f'\n    <metadata key="{k}" value="{v}"/>' for k, v in kv.items())
                    return re.sub(r'<metadata key="name" value="[^"]*"/>', lambda n_: head, m.group(0), count=1)
                data = re.sub(r"<object id=.*?</object>", add, data.decode(), flags=re.S).encode()
                if todo:
                    sys.exit("fewer objects in the project than in the assemble list")
            zout.writestr(item, data)


def repack(src, dst):
    """Smaller file: the CLI stores the G-code almost uncompressed."""
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zout:
        for item in zin.infolist():
            zi = zipfile.ZipInfo(item.filename, date_time=item.date_time)
            zi.compress_type = zipfile.ZIP_DEFLATED
            zout.writestr(zi, zin.read(item.filename), compresslevel=9)


def summary(path):
    with zipfile.ZipFile(path) as z:
        info = z.read("Metadata/slice_info.config").decode()
        names = re.findall(r'key="plater_name" value="([^"]*)"', z.read("Metadata/model_settings.config").decode())
    lines = []
    for i, plate in enumerate(re.findall(r"<plate>.*?</plate>", info, flags=re.S)):
        secs = int(re.search(r'key="prediction" value="(\d+)"', plate).group(1))
        fil = ", ".join(f"{t} {c} {float(g):.1f} g" for t, c, g in
                        re.findall(r'<filament id="\d+"[^>]*type="([^"]+)" color="([^"]+)"[^>]*used_g="([\d.]+)"', plate))
        supports = re.search(r'key="support_used" value="(\w+)"', plate).group(1)
        lines.append(f"  plate {i + 1} {names[i]!r}: {secs // 3600}h{secs % 3600 // 60:02d}m, {fil}, supports {supports}")
    return "\n".join(lines)


def build(st, name, process, filaments, colours, purge, plates, bed):
    """plates: [(plate name, [(stl, filament, group, settings), ...]), ...]"""
    s = lambda f: os.path.abspath(os.path.join(STL, f))
    settings, spec = [], []
    for plate_name, parts in plates:
        objs, groups = [], set()
        for stl, filament, group, kv in parts:
            if group not in groups:  # the first part of a group decides the object's name, filament and settings
                groups.add(group)
                settings.append({"name": NAMES[stl.split("-")[-1]], "extruder": str(filament), **kv})
            objs.append({"path": s(stl), "count": 1, "filaments": [filament], "assemble_index": [group],
                         "pos_x": [90], "pos_y": [90], "pos_z": [0], "subtype": "normal_part"})
        spec.append({"plate_name": plate_name, "need_arrange": True, "objects": objs})
    listing = os.path.join(st.work, name + ".json")
    with open(listing, "w") as f:
        json.dump({"plates": spec}, f)

    raw, patched, sliced = (os.path.join(st.work, f"{name}-{x}.3mf") for x in ("raw", "patched", "sliced"))
    st.run("--load-settings", ";".join([st.preset(MACHINE, "printer_settings_id"), st.preset(process, "print_settings_id")]),
           "--load-filaments", ";".join(st.preset(f, "filament_settings_id") for f in filaments),
           "--load-assemble-list", listing, "--orient", "0", "--export-3mf", raw)
    patch_project(raw, patched, colours, purge, bed, settings)
    st.run("--slice", "0", "--export-3mf", sliced, patched)
    final = os.path.join(OUT, name + ".3mf")
    repack(sliced, final)
    print(f"{os.path.relpath(final)} ({os.path.getsize(final) / 1e6:.1f} MB)\n{summary(final)}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--studio")
    ap.add_argument("--bed", default="Textured PEI Plate", help='e.g. "Textured PEI Plate", "High Temp Plate", "Cool Plate"')
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    p = "agenttally-cyd28-"
    with tempfile.TemporaryDirectory() as work:
        st = Studio(find_studio(a.studio), work)
        # Filament 1 is the dark one because the CLI records a wrong filament and an empty weight
        # for a plate whose only filament is not number 1.
        build(st, "agenttally-a1mini", "0.20mm Standard @BBL A1M", [PLA, PLA], [DARK, WHITE],
              [[0, PURGE_DARK_TO_WHITE], [PURGE_WHITE_TO_DARK, 0]],
              [("Body", [(p + "front.stl", 1, 1, BODY), (p + "back.stl", 1, 2, BODY), (p + "plate.stl", 1, 3, BODY),
                         (p + "light_pipe.stl", 1, 4, {})]),
               ("Stand, 2 colours (AMS)", [(p + "stand.stl", 2, 1, STAND), (p + "stand_text.stl", 1, 1, STAND)]),
               ("Stand, 1 colour", [(p + "stand.stl", 1, 1, STAND)])],
              a.bed)
        build(st, "agenttally-lightpipe", "0.12mm Fine @BBL A1M", [PETG], [CLEAR], None,
              [("Light pipes, clear PETG", [(p + "light_pipe.stl", 1, 1, PIPE), (p + "light_pipe.stl", 1, 2, PIPE)])],
              a.bed)


if __name__ == "__main__":
    main()
