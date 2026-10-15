#!/bin/sh
# Render STL files and preview images with the official OpenSCAD docker image.
#   ./render.sh            all parts for every board + previews
#   ./render.sh preview    previews only
set -e
cd "$(dirname "$0")"
# Uses $OPENSCAD (or `openscad` on PATH); falls back to a docker image with xvfb.
OPENSCAD=${OPENSCAD:-$(command -v openscad || true)}
if [ -n "$OPENSCAD" ]; then
  run() { "$OPENSCAD" --backend=Manifold "$@" 2>&1 | grep -i -E "error|warning" || true; }
else
  IMG=agenttally-openscad
  docker image inspect $IMG >/dev/null 2>&1 || docker build -q -t $IMG .
  run() { docker run --rm -v "$PWD":/work -w /work $IMG xvfb-run -a openscad --backend=Manifold "$@" 2>&1 | grep -i -E "error|warning" || true; }
fi
mkdir -p stl img
if [ "$1" != "preview" ]; then
  for b in cyd28 cyd35; do
    for p in front back stand plate; do
      run --export-format=binstl -o stl/agenttally-$b-$p.stl -D "part=\"$p\"" -D "board=\"$b\"" agenttally.scad
    done
  done
fi
run -o img/assembly.png --imgsize=1200,1500 --colorscheme=Tomorrow --camera=0,15,62,62,0,322,330 --viewall -D 'part="assembly"' agenttally.scad
run -o img/assembly-back.png --imgsize=1200,1500 --colorscheme=Tomorrow --camera=0,15,62,58,0,150,330 --viewall -D 'part="assembly"' agenttally.scad
run -o img/section.png --imgsize=1300,1300 --colorscheme=Tomorrow --camera=0,15,62,90,0,90,300 --projection=o --viewall -D 'part="section"' agenttally.scad
run -o img/parts.png --imgsize=1800,1000 --colorscheme=Tomorrow --camera=0,0,0,55,0,25,0 --autocenter --viewall -D 'part="print_all"' agenttally.scad
