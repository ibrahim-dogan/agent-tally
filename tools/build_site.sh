#!/bin/sh
# Assemble the GitHub Pages site (web flasher) into dist/.
#   tools/build_site.sh            expects web/firmware (tools/build_firmware.sh) and enclosure/stl
set -e
cd "$(dirname "$0")/.."
rm -rf dist && mkdir -p dist/img dist/stl
cp -R web/index.html web/manifest web/firmware dist/
cp docs/img/screen-*.png dist/img/
cp enclosure/img/assembly.png dist/img/
cp enclosure/stl/*.stl dist/stl/
touch dist/.nojekyll
echo "site: dist/ ($(du -sh dist | cut -f1))"
