#!/bin/sh
# Fetch the pinned LVGL release into third_party/lvgl (sources only), checking its hash.
set -e
VER=9.3.0
SHA256=4933becfd3603b29158a5d04138139582836ef2bc17beb6c39dccda9cb0d32e7
cd "$(dirname "$0")/.."
[ -f third_party/lvgl/lvgl.h ] && grep -q "LVGL_VERSION_MAJOR 9" third_party/lvgl/lv_version.h && exit 0
mkdir -p third_party
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
curl -fsSL -o "$TMP/lvgl.tar.gz" "https://github.com/lvgl/lvgl/archive/refs/tags/v$VER.tar.gz"
echo "$SHA256  $TMP/lvgl.tar.gz" | sha256sum -c - 2>/dev/null || echo "$SHA256  $TMP/lvgl.tar.gz" | shasum -a 256 -c -
tar xzf "$TMP/lvgl.tar.gz" -C "$TMP" "lvgl-$VER/src" "lvgl-$VER/lvgl.h" "lvgl-$VER/lv_version.h" "lvgl-$VER/lvgl_private.h" "lvgl-$VER/LICENCE.txt"
rm -rf third_party/lvgl
mv "$TMP/lvgl-$VER" third_party/lvgl
find third_party/lvgl -name '._*' -delete
echo "LVGL $VER in third_party/lvgl"
