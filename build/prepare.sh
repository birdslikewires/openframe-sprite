#!/bin/sh
# Prepares the sprite-build image, in an i386 debian:trixie container: the build tools and
# libraries, then the Mesa-free display stack (stack.sh). Used by the Dockerfile, and by CI,
# which runs it with 'docker run' instead (see .github/workflows/package.yml).
set -e
here=$(dirname "$0")
export DEBIAN_FRONTEND=noninteractive
sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/debian.sources
apt-get update -q
apt-get install -y -q --no-install-recommends \
	build-essential meson ninja-build cmake pkg-config dpkg-dev ca-certificates curl file \
	libwayland-dev wayland-protocols libdrm-dev libinput-dev libseat-dev libudev-dev libpixman-1-dev \
	libxkbcommon-dev libdisplay-info-dev hwdata libliftoff-dev libxcb1-dev libxcb-composite0-dev \
	libxcb-icccm4-dev libxcb-render0-dev libxcb-res0-dev libxcb-xfixes0-dev libxcb-ewmh-dev \
	libxcb-errors-dev glslang-tools xutils-dev libxfont-dev libxkbfile-dev libxshmfence-dev libxcvt-dev \
	libtirpc-dev libxau-dev libxdmcp-dev x11proto-dev xtrans-dev libbsd-dev libmd-dev nettle-dev \
	libssl-dev libsystemd-dev libpciaccess-dev libasound2-dev libx11-dev libxext-dev libxrandr-dev \
	libxcursor-dev libxfixes-dev libxi-dev libxss-dev libsamplerate0-dev libjpeg62-turbo-dev libpng-dev
sh "$here/stack.sh"
rm -rf /var/lib/apt/lists/*
