#!/bin/sh
set -ex
export DEBIAN_FRONTEND=noninteractive
sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/debian.sources
apt-get update -q
apt-get install -y -q --no-install-recommends build-essential cmake pkg-config dpkg-dev ca-certificates \
  libwayland-dev wayland-protocols libxkbcommon-dev libasound2-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev \
  libxfixes-dev libxi-dev libxss-dev libsamplerate0-dev libudev-dev
mkdir -p /bld && cd /bld && cp /src/*.c /bld/
apt-get source -q libsdl2
apt-get source -q sdl12-compat
P=/opt/sprite
cd /bld/libsdl2-2*/ && rm -rf b && cmake -S . -B b -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release -DSDL_STATIC=OFF -DSDL_TEST=OFF \
  -DSDL_KMSDRM=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_VULKAN=OFF -DSDL_PULSEAUDIO=OFF -DSDL_PIPEWIRE=OFF -DSDL_JACK=OFF \
  -DSDL_SNDIO=OFF -DSDL_LIBDECOR=OFF -DSDL_WAYLAND=OFF -DSDL_X11=ON -DSDL_ALSA=ON -DSDL_ESD=OFF -DSDL_ARTS=OFF -DSDL_NAS=OFF -DSDL_DIRECTFB=OFF -DSDL_OFFSCREEN=ON \
  > /out/sdl2-cmake.log 2>&1 || { tail -40 /out/sdl2-cmake.log; exit 1; }
grep -iE "wayland|x11|opengl|egl|kmsdrm|alsa" /out/sdl2-cmake.log | head -30
cmake --build b -j8 && cmake --install b
cd /bld/sdl12-compat-*/ && rm -rf b && cmake -S . -B b -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release -DSDL12TESTS=OFF -DCMAKE_PREFIX_PATH=$P && cmake --build b -j8 && cmake --install b
cd /bld && gcc -O2 -o sdl12test sdl12test.c -I$P/include -L$P/lib -lSDL -Wl,-rpath,$P/lib && gcc -O2 -o x11test x11test.c -lX11
cd $P && tar czf /out/sprite-sdl.tgz lib bin 2>/dev/null; cp /bld/sdl12test /bld/x11test /out/; ls -la /out
