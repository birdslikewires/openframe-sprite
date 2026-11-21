#!/bin/sh
# Build sprite for the OpenFrame (i386) in a container. Needs docker (colima on the Mac).
#   scripts/build.sh            build out/sprite
#   scripts/build.sh package    build the package, out/openframe-sprite_<version>_i386.deb
# The repository must be under $HOME when using colima, which only shares the home directory.
set -e
cd "$(dirname "$0")/.."
sh scripts/fetch-lvgl.sh
docker build --platform linux/386 -q -t sprite-build build/ >/dev/null
if [ "$1" = package ]; then
	version=$(bash package/version.sh)
	docker run --rm --platform linux/386 -v "$PWD":/src sprite-build sh package/build.sh "$version" out
else
	docker run --rm --platform linux/386 -v "$PWD":/src sprite-build make -j"$(nproc)"
	ls -la out/sprite
fi
