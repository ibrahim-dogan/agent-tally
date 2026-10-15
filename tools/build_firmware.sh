#!/bin/sh
# Build every board and write single-file images (bootloader + partitions + app)
# for the web flasher into web/firmware/, plus ESP Web Tools manifests.
#   tools/build_firmware.sh            all boards
#   tools/build_firmware.sh cyd28      one board
set -e
cd "$(dirname "$0")/.."
BOARDS=${*:-"cyd28 cyd24r cyd24c cyd32r cyd32c cyd35r cyd35c"}
VERSION=$(sed -n 's/.*AT_FW_VERSION=\\"\([^\\]*\)\\".*/\1/p' firmware/platformio.ini | head -1)
BOOT_APP0="$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
mkdir -p web/firmware web/manifest
for b in $BOARDS; do
  (cd firmware && pio run -s -e "$b")
  B=firmware/.pio/build/$b
  pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32 merge_bin -o "web/firmware/agenttally-$b.bin" \
    --flash_mode dio --flash_freq 40m --flash_size 4MB \
    0x1000 "$B/bootloader.bin" 0x8000 "$B/partitions.bin" 0xe000 "$BOOT_APP0" 0x10000 "$B/firmware.bin" >/dev/null
  cat > "web/manifest/$b.json" <<JSON
{
  "name": "AgentTally ($b)",
  "version": "$VERSION",
  "new_install_prompt_erase": true,
  "builds": [{ "chipFamily": "ESP32", "parts": [{ "path": "../firmware/agenttally-$b.bin", "offset": 0 }] }]
}
JSON
  echo "$b -> web/firmware/agenttally-$b.bin ($(wc -c < "web/firmware/agenttally-$b.bin" | tr -d ' ') bytes)"
done
