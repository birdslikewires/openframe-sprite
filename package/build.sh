#!/bin/sh
# Builds the openframe-sprite package: sprite, and the Mesa-free display stack it runs on
# (wlroots, Xwayland, and SDL for apps), all under /opt/sprite. Run in the sprite-build image
# (build/Dockerfile, which has the stack built in /opt/sprite), from the repository's root:
#   sh package/build.sh <version> <output folder>

set -eu

version=$1
out=$2
PKGNAME=openframe-sprite
P=/opt/sprite
multiarch=$(dpkg-architecture -qDEB_HOST_MULTIARCH)

rm -f out/obj/src/*.o   # sprite's own sources, so the version goes in
make -j"$(nproc)" VERSION="$version" > /dev/null

work=$(mktemp -d)
pkg=$work/pkg
install -d "$pkg/DEBIAN" "$pkg$P/bin" "$pkg$P/libexec" "$pkg$P/lib/$multiarch" "$pkg$P/share/sprite/apps" \
  "$pkg/usr/lib/systemd/system" "$pkg/usr/share/doc/$PKGNAME"

install -m 755 out/sprite "$pkg$P/bin/sprite"
install -m 755 "$P/bin/Xwayland" "$pkg$P/bin/Xwayland"
install -m 755 system/sprite-helper "$pkg$P/libexec/sprite-helper"
install -m 755 system/sprite-ping "$pkg$P/libexec/sprite-ping"
cp -a "$P/lib/$multiarch/libwlroots-0.18.so" "$pkg$P/lib/$multiarch/"
cp -a "$P"/lib/libSDL2-2.0.so* "$P"/lib/libSDL-1.2.so* "$pkg$P/lib/"
rm -f "$pkg$P"/lib/*.so.*.*.*.debug
install -m 644 settings/interface.txt "$pkg$P/share/sprite/interface.txt"
install -m 644 apps/settings.app "$pkg$P/share/sprite/apps/"
install -m 644 system/sprite.service system/sprite-helper.socket system/sprite-helper@.service \
  system/sprite-ping.service system/sprite-ping.timer \
  "$pkg/usr/lib/systemd/system/"
install -D -m 644 system/sprite.sysusers "$pkg/usr/lib/sysusers.d/openframe-sprite.conf"
install -D -m 644 system/sprite.tmpfiles "$pkg/usr/lib/tmpfiles.d/openframe-sprite.conf"
for f in postinst prerm postrm; do install -m 755 "package/$f" "$pkg/DEBIAN/$f"; done
find "$pkg$P" -type f \( -name '*.so*' -o -perm -u+x \) ! -name sprite-helper ! -name sprite-ping -exec strip --strip-unneeded {} +

# Licences: sprite's own, and those of what's built into or shipped with it.
{
  cat package/copyright
  for l in wlroots xwayland sdl2 sdl12-compat; do
    printf '\n\n==== %s ====\n\n' "$l"; cat "$P/share/licences/$l"
  done
  printf '\n\n==== lvgl ====\n\n'; cat third_party/lvgl/LICENCE.txt
  printf '\n\n==== lucide ====\n\n'; cat src/icons-lucide-LICENSE
  printf '\n\n==== Montserrat and Font Awesome Free 5 (fonts) ====\n\n'; cat package/licences/OFL-1.1.txt
} > "$pkg/usr/share/doc/$PKGNAME/copyright"

# Dependencies from the binaries, beyond the libraries the package carries itself.
mkdir -p "$work/deb/debian"
printf 'Source: %s\n\nPackage: %s\nArchitecture: i386\n' "$PKGNAME" "$PKGNAME" > "$work/deb/debian/control"
bins=$(find "$pkg$P" -type f \( -name '*.so*' -o -path '*/bin/*' \))
deps=$(cd "$work/deb" && dpkg-shlibdeps -O --ignore-missing-info -l"$pkg$P/lib/$multiarch" -l"$pkg$P/lib" $bins 2>/dev/null \
  | sed -n 's/^shlibs:Depends=//p')
[ -n "$deps" ] || { echo "dpkg-shlibdeps found no dependencies"; exit 1; }
# Not linked, but needed: Xwayland runs xkbcomp on the keymap; SDL loads its X11 and ALSA
# libraries when an app starts it; and the helper and settings use the OpenFrame scripts
# (bootrw, of-backlight).
# openframe-scripts from the version with of-audio's holder= lines, found quickly (apps
# restarted as the sound output changes), after of-audio (the Audio tab), of-backlight resume
# <min> <max> (the brightness slider let go), name-settings (the name, on the About tab),
# of-backlight show and set, of-update set-time, and of-backlight sleep and wake (display
# sleep, 1:26.11.153):
# so apt holds this Sprite back on a unit until that's released too, whatever order they're
# promoted in. Raise it when Sprite starts to need something newer from it.
deps="$deps, x11-xkb-utils, xkb-data, libx11-6, libxext6, libxcursor1, libxfixes3, libxi6, libxrandr2, libxss1, libasound2t64, openframe-scripts (>= 1:26.11.153), curl, iproute2"

size=$(du -sk "$pkg/opt" "$pkg/usr" | awk '{ s += $1 } END { print s }')
cat > "$pkg/DEBIAN/control" <<CONTROL
Package: $PKGNAME
Version: $version
Architecture: i386
Section: x11
Priority: optional
Maintainer: OpenFrame Project <noreply@openbeak.net>
Installed-Size: $size
Depends: $deps
Homepage: https://github.com/birdslikewires/openframe-sprite
Description: Sprite, the OpenFrame touch shell
 A touch shell for the OpenFrame's 800x480 screen: a home screen of apps, settings, and one
 app at a time full screen, drawn in software. It's a Wayland compositor (wlroots) and the
 shell (LVGL) in one program, with Xwayland for X11 and SDL apps, built without Mesa.
 Installing it puts sprite on tty1 in place of the login prompt; removing it puts the
 prompt back.
CONTROL

deb="$out/${PKGNAME}_${version}_i386.deb"
mkdir -p "$out"
dpkg-deb --root-owner-group -Zxz --build "$pkg" "$deb" > /dev/null
dpkg-deb -I "$deb" | sed -n '/Package:/,$p'
ls -l "$deb"
