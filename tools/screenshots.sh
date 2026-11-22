#!/bin/sh
# Render UI screenshots with the desktop simulator against a throwaway bridge.
#   tools/screenshots.sh [W H]     default 240 320
set -e
cd "$(dirname "$0")/.."
W=${1:-240}; H=${2:-320}
OUT=docs/img; mkdir -p "$OUT"
HP=47491; SP=47492
export HOME_BAK="$HOME"
TMPH=$(mktemp -d)
# keep the throwaway bridge from starting a second mouse-pointer helper (macOS)
mkdir -p "$TMPH/.agent-tally"
echo '{"pointer": {"enabled": false}}' > "$TMPH/.agent-tally/config.json"
# pretend it is around noon, so night mode never blanks the shots
TZ="XXX$(( $(date -u +%H | sed 's/^0//') - 12 ))"; export TZ
HOME=$TMPH AGENTTALLY_NAME=studio python3 host/agenttally.py --http-port $HP serve --no-serial --sim-port $SP >/dev/null 2>&1 &
BR=$!
trap 'kill $BR 2>/dev/null; rm -rf "$TMPH"' EXIT
sleep 1
(cd firmware && PLATFORMIO_BUILD_FLAGS="-DSIM_W=$W -DSIM_H=$H" pio run -s -e sim)
SIM=firmware/.pio/build/sim/program
sfx=""; [ "$W" != 240 ] && sfx="-${W}"
# offline + empty states first
HOME=$TMPH $SIM --port 1 --shot "$OUT/screen-offline$sfx.ppm@1500" --exit 1800
HOME=$TMPH $SIM --port $SP --shot "$OUT/screen-empty$sfx.ppm@1500" --exit 1800
HOME=$TMPH python3 host/agenttally.py --http-port $HP demo >/dev/null
sleep 0.5
k=$((W * 1000 / 240))
y2=$((150 * k / 1000)); xs=$((W - 16 * k / 1000)); yh=$((18 * k / 1000)); xb=$((30 * k / 1000))
# The sessions arriving raise an alert (frame + card glow, three 1.4 s pulses):
# catch it near a peak, then take the other shots once it is over.
HOME=$TMPH $SIM --port $SP --shot "$OUT/screen-alert$sfx.ppm@2200" \
  --shot "$OUT/screen-list$sfx.ppm@5500" \
  --tap $((W/2)),$y2@5700 --shot "$OUT/screen-detail$sfx.ppm@6300" \
  --tap $xb,$yh@6500 --tap $xs,$yh@6800 --shot "$OUT/screen-settings$sfx.ppm@7300" --exit 7500
for f in "$OUT"/*.ppm; do sips -s format png "$f" --out "${f%.ppm}.png" >/dev/null && rm "$f"; done
ls "$OUT"
