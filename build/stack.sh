#!/bin/sh
# Builds the Mesa-free display stack into /opt/sprite, inside the i386 Trixie build image:
#   wlroots 0.18 with the pixman renderer only, Xwayland without glamor or GLX,
#   SDL2 with X11 and ALSA only, and sdl12-compat on top of it.
# Debian's own builds of these depend on Mesa (about 250 MB with LLVM); see spike/README.md.
set -ex
P=/opt/sprite
export PKG_CONFIG_PATH=$P/lib/i386-linux-gnu/pkgconfig:$P/lib/pkgconfig
mkdir -p /bld && cd /bld
apt-get source -q xwayland wlroots libsdl2 sdl12-compat
cd /bld/xwayland-*/
meson setup b --prefix=$P --buildtype=release -Dglamor=false -Dglx=false -Dlibdecor=false \
	-Dsecure-rpc=false -Dxvfb=false -Dxwayland_ei=false -Ddocs=false -Ddevel-docs=false -Dxselinux=false \
	-Dxkb_dir=/usr/share/X11/xkb -Dxkb_output_dir=/var/lib/xkb -Dxkb_bin_dir=/usr/bin \
	-Ddefault_font_path=/usr/share/fonts/X11/misc
ninja -C b install
cd /bld/wlroots-0.18*/
meson setup b --prefix=$P --buildtype=release -Drenderers= -Dallocators= -Dbackends=drm,libinput \
	-Dxwayland=enabled -Dexamples=false -Dsession=enabled -Dcolor-management=disabled
ninja -C b install
cd /bld/libsdl2-2*/
cmake -S . -B b -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release -DSDL_STATIC=OFF -DSDL_TEST=OFF \
	-DSDL_KMSDRM=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF -DSDL_PULSEAUDIO=OFF \
	-DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF -DSDL_LIBDECOR=OFF -DSDL_WAYLAND=OFF -DSDL_X11=ON \
	-DSDL_ALSA=ON -DSDL_DIRECTFB=OFF -DSDL_OFFSCREEN=ON
cmake --build b -j"$(nproc)" && cmake --install b
cd /bld/sdl12-compat-*/
cmake -S . -B b -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release -DSDL12TESTS=OFF -DCMAKE_PREFIX_PATH=$P
cmake --build b -j"$(nproc)" && cmake --install b
rm -rf /bld
