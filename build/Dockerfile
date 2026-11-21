# i386 Debian Trixie build environment for sprite and its Mesa-free display stack.
# Build with: docker build --platform linux/386 -t sprite-build build/
FROM i386/debian:trixie
ENV DEBIAN_FRONTEND=noninteractive
RUN sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/debian.sources && \
    apt-get update -q && apt-get install -y -q --no-install-recommends \
	build-essential meson ninja-build cmake pkg-config dpkg-dev ca-certificates curl \
	libwayland-dev wayland-protocols libdrm-dev libinput-dev libseat-dev libudev-dev libpixman-1-dev \
	libxkbcommon-dev libdisplay-info-dev hwdata libliftoff-dev libxcb1-dev libxcb-composite0-dev \
	libxcb-icccm4-dev libxcb-render0-dev libxcb-res0-dev libxcb-xfixes0-dev libxcb-ewmh-dev \
	libxcb-errors-dev glslang-tools xutils-dev libxfont-dev libxkbfile-dev libxshmfence-dev libxcvt-dev \
	libtirpc-dev libxau-dev libxdmcp-dev x11proto-dev xtrans-dev libbsd-dev libmd-dev nettle-dev \
	libssl-dev libsystemd-dev libpciaccess-dev libasound2-dev libx11-dev libxext-dev libxrandr-dev \
	libxcursor-dev libxfixes-dev libxi-dev libxss-dev libsamplerate0-dev file && \
    rm -rf /var/lib/apt/lists/*
COPY stack.sh /stack.sh
RUN apt-get update -q && sh /stack.sh && rm -rf /var/lib/apt/lists/*
WORKDIR /src
