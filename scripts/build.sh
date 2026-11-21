#!/bin/sh
# Build sprite for the OpenFrame (i386) in a container. Needs docker (colima on the Mac).
#   scripts/build.sh          build out/sprite
#   scripts/build.sh stack    also write out/sprite-stack.tgz (/opt/sprite: wlroots, Xwayland, SDL)
# The repository must be under $HOME when using colima, which only shares the home directory.
set -e
cd "$(dirname "$0")/.."
sh scripts/fetch-lvgl.sh
docker build --platform linux/386 -q -t sprite-build build/ >/dev/null
docker run --rm --platform linux/386 -v "$PWD":/src sprite-build sh -c 'make -j"$(nproc)" && strip -o out/sprite.stripped out/sprite'
if [ "$1" = stack ]; then
	docker run --rm --platform linux/386 -v "$PWD":/src sprite-build \
		sh -c 'tar czf out/sprite-stack.tgz -C /opt/sprite --exclude=include --exclude=share/man --exclude="*.a" .'
fi
ls -la out/sprite out/sprite.stripped
