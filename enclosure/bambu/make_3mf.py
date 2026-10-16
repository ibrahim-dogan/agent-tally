#!/usr/bin/env python3
"""Build ready-to-print Bambu Studio projects (A1 mini, 0.4 mm nozzle) from the STLs.

    python3 enclosure/bambu/make_3mf.py [--studio /path/to/BambuStudio.app] [--bed "Textured PEI Plate"]

Writes enclosure/print/:
  agenttally-body.3mf        front + back + bottom plate        dark PLA
  agenttally-stand.3mf       stand + a white PLA light pipe       white PLA
  agenttally-stand-2color.3mf  same, letters inlaid in dark PLA (AMS); not pre-sliced
  agenttally-lightpipe.3mf   two light pipes                     clear PETG, 100% infill

Filament slots are only colour hints: Bambu Studio maps them to your AMS when you send the job.
"""

import argparse
import glob
import json
import os
import shutil
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


def process(name, **overrides):
    p = {"type": "process", "name": name, "inherits": overrides.pop("base", "0.20mm Standard @BBL A1M"),
         "from": "User", "print_settings_id": name, "compatible_printers": [MACHINE], "enable_support": "0"}
    p.update({k: str(v) for k, v in overrides.items()})
    return p


class Studio:
    def __init__(self, app, work):
        self.bin = os.path.join(app, "Contents/MacOS/BambuStudio")
        self.idx = index(os.path.join(app, "Contents/Resources/profiles/BBL"))
        self.work = work

    def flat(self, spec):
        data = spec if isinstance(spec, dict) else self.idx[spec]
        flat = resolve(data, self.idx)
        flat["from"] = "User" if isinstance(spec, dict) else "system"
        path = os.path.join(self.work, flat["name"].replace("/", "_") + ".json")
        with open(path, "w") as f:
            json.dump(flat, f)
        return path

    def run(self, *args):
        r = subprocess.run([self.bin, *args], capture_output=True, text=True)
        if r.returncode != 0:
            errs = [l for l in (r.stdout + r.stderr).splitlines() if "[error]" in l]
            sys.exit(f"Bambu Studio failed ({r.returncode}): " + "\n".join(errs[-5:]))


def patch_project(path, colours, bed):
    """Set filament colours and the bed type inside a 3MF project."""
    tmp = path + ".tmp"
    with zipfile.ZipFile(path) as zin, zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as zout:
        for item in zin.infolist():
            data = zin.read(item.filename)
            if item.filename == "Metadata/project_settings.config":
                cfg = json.loads(data)
                # The CLI leaves some per-filament lists at one entry when several filaments are
                # loaded from STL input; pad them so every list has one entry per filament.
                n = len(cfg.get("filament_settings_id") or [1])
                for k, v in cfg.items():
                    if k.startswith("filament_") and isinstance(v, list) and len(v) == 1 and n > 1:
                        cfg[k] = v * n
                cfg["filament_colour"] = colours
                cfg["curr_bed_type"] = bed
                data = json.dumps(cfg, indent=4).encode()
            zout.writestr(item, data)
    os.replace(tmp, path)


def summary(path):
    with zipfile.ZipFile(path) as z:
        info = z.read("Metadata/slice_info.config").decode()
    import re
    secs = sum(int(x) for x in re.findall(r'key="prediction" value="(\d+)"', info))
    grams = re.findall(r'<filament id="(\d+)"[^>]*type="([^"]+)" color="([^"]+)"[^>]*used_g="([\d.]+)"', info)
    fil = ", ".join(f"{t} {c} {float(g):.0f} g" for _, t, c, g in grams)
    return f"{secs // 3600}h{secs % 3600 // 60:02d}m, {fil}"


def build(st, name, settings, filaments, colours, files, bed, ids=None, assemble=False, slice_it=True):
    load = ["--load-settings", ";".join(settings), "--load-filaments", ";".join(filaments)]
    if ids:
        load += ["--load-filament-ids", ids]
    if assemble:
        load.append("--assemble")
    st.run(*load, "--arrange", "1", "--orient", "0", "--outputdir", st.work, "--export-3mf", name + "-raw.3mf", *files)
    raw = os.path.join(st.work, name + "-raw.3mf")
    patch_project(raw, colours, bed)
    final = os.path.join(OUT, name + ".3mf")
    if not slice_it:
        # Multi-colour G-code export fails in the CLI; Bambu Studio's GUI slices it fine.
        shutil.copy(raw, final)
        print(f"{os.path.relpath(final)}: not sliced, open it in Bambu Studio and press Slice")
        return
    st.run("--slice", "0", "--outputdir", st.work, "--export-3mf", name + ".3mf", raw)
    shutil.copy(os.path.join(st.work, name + ".3mf"), final)
    print(f"{os.path.relpath(final)}: {summary(final)}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--studio")
    ap.add_argument("--bed", default="High Temp Plate", help='e.g. "Textured PEI Plate", "High Temp Plate", "Cool Plate"')
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    with tempfile.TemporaryDirectory() as work:
        st = Studio(find_studio(a.studio), work)
        machine = st.flat(MACHINE)
        pla, petg = st.flat(PLA), st.flat(PETG)
        s = lambda f: os.path.join(STL, f)

        body = st.flat(process("AgentTally body", wall_loops=3, sparse_infill_density="15%"))
        build(st, "agenttally-body", [machine, body], [pla], [DARK], [s("agenttally-cyd28-front.stl"),
              s("agenttally-cyd28-back.stl"), s("agenttally-cyd28-plate.stl")], a.bed)

        stand = st.flat(process("AgentTally stand", wall_loops=4, sparse_infill_density="40%",
                                sparse_infill_pattern="gyroid"))
        build(st, "agenttally-stand", [machine, stand], [pla], [WHITE],
              [s("agenttally-cyd28-stand.stl"), s("agenttally-cyd28-light_pipe.stl")], a.bed)
        # optional two-colour stand: letters inlaid in the second filament
        build(st, "agenttally-stand-2color", [machine, stand], [pla, pla], [WHITE, DARK],
              [s("agenttally-cyd28-stand.stl"), s("agenttally-cyd28-stand_text.stl"),
               s("agenttally-cyd28-light_pipe_beside.stl")], a.bed, ids="1,2,1", assemble=True, slice_it=False)

        pipe = st.flat(process("AgentTally light pipe", base="0.12mm Fine @BBL A1M", wall_loops=4,
                               sparse_infill_density="100%", sparse_infill_pattern="zig-zag",
                               outer_wall_speed=60, inner_wall_speed=80))
        build(st, "agenttally-lightpipe", [machine, pipe], [petg], [CLEAR],
              [s("agenttally-cyd28-light_pipe.stl"), s("agenttally-cyd28-light_pipe.stl")], a.bed)


if __name__ == "__main__":
    main()
