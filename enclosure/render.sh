#!/bin/sh
# Render the STL files and the preview images.
#   ./render.sh            all parts for every board + previews
#   ./render.sh preview    previews only
set -e
cd "$(dirname "$0")"
# Uses $OPENSCAD (or `openscad` on PATH); falls back to a docker image with xvfb.
OPENSCAD=${OPENSCAD:-$(command -v openscad || true)}
if [ -n "$OPENSCAD" ]; then
  run() { "$OPENSCAD" --backend=Manifold "$@" 2>&1 | grep -i -E "error|warning" | grep -v NoError || true; }
else
  IMG=agenttally-openscad
  docker image inspect $IMG >/dev/null 2>&1 || docker build -q -t $IMG .
  run() { docker run --rm -v "$PWD":/work -w /work $IMG xvfb-run -a openscad --backend=Manifold "$@" 2>&1 | grep -i -E "error|warning" | grep -v NoError || true; }
fi
mkdir -p stl img
if [ "$1" != "preview" ]; then
  for b in cyd28 cyd35; do
    for p in front back stand plate; do
      run --export-format=binstl -o stl/agenttally-$b-$p.stl -D "part=\"$p\"" -D "board=\"$b\"" agenttally.scad
    done
  done
  # plain stand, no lettering
  run --export-format=binstl -o stl/agenttally-cyd28-stand-plain.stl -D 'part="stand"' -D 'label=""' agenttally.scad
  # experimental: bezel and rear cover with the light pipe slot, and the pipe itself
  for p in front back; do
    run --export-format=binstl -o stl/agenttally-cyd28-$p-lightpipe.stl -D "part=\"$p\"" -D 'light_pipe=true' agenttally.scad
  done
  run --export-format=binstl -o stl/agenttally-cyd28-light_pipe.stl -D 'part="light_pipe"' -D 'light_pipe=true' agenttally.scad
  run --export-format=binstl -o stl/agenttally-cyd28-stand_text.stl -D 'part="stand_text"' agenttally.scad
fi
run -o img/assembly.png --imgsize=1200,1500 --colorscheme=Tomorrow --camera=0,15,62,62,0,322,330 --viewall -D 'part="assembly"' agenttally.scad
run -o img/assembly-back.png --imgsize=1200,1500 --colorscheme=Tomorrow --camera=0,15,62,58,0,150,330 --viewall -D 'part="assembly"' agenttally.scad
run -o img/section.png --imgsize=1300,1300 --colorscheme=Tomorrow --camera=0,15,62,90,0,90,300 --projection=o --viewall -D 'part="section"' agenttally.scad
run -o img/light-section.png --imgsize=1200,600 --colorscheme=Tomorrow --camera=30,60,-2,90,0,0,90 --projection=o -D 'part="light_section"' -D 'light_pipe=true' agenttally.scad
run -o img/parts.png --imgsize=1800,900 --colorscheme=Tomorrow --camera=14,42,0,48,0,15,570 -D 'part="print_all"' agenttally.scad
run -o img/exploded.png --imgsize=1100,1300 --colorscheme=Tomorrow --camera=25,-43,28,62,0,35,330 -D 'part="exploded"' agenttally.scad
