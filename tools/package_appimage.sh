#!/bin/sh
# Linux AppImage from a `linux` preset build (see docs/build/linux.md).
# Usage: tools/package_appimage.sh [build-dir] [output-dir]
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
build=${1:-$HOME/heroeslore-linux}
out=${2:-$build}
version=$(sed -n 's/^project(heroes_lore_port VERSION \([0-9.]*\).*/\1/p' "$root/cpp/CMakeLists.txt")
tool_version=1-alpha-20251107-1
tool="$build/linuxdeploy-x86_64.AppImage"
if [ ! -x "$tool" ]; then
    wget -q -O "$tool" "https://github.com/linuxdeploy/linuxdeploy/releases/download/$tool_version/linuxdeploy-x86_64.AppImage"
    chmod +x "$tool"
fi
icon="$root/cpp/dist/linux/heroes_lore.png"
[ -f "$root/cpp/assets/linux/heroes_lore.png" ] && icon="$root/cpp/assets/linux/heroes_lore.png" # local art, 256x256
appdir="$build/AppDir"
rm -rf "$appdir"
mkdir -p "$appdir/usr/share/doc/heroes_lore"
cp "$root/cpp/dist/README.txt" "$root/cpp/dist/SOUNDFONT-LICENSE.txt" "$appdir/usr/share/doc/heroes_lore/"
cd "$out"
# FUSE is not needed to run linuxdeploy itself (containers, WSL).
APPIMAGE_EXTRACT_AND_RUN=1 LINUXDEPLOY_OUTPUT_VERSION="$version" "$tool" --appdir "$appdir" \
    --executable "$build/heroes_lore" \
    --desktop-file "$root/cpp/dist/linux/heroes_lore.desktop" \
    --icon-file "$icon" \
    --output appimage
mv -f "Heroes_Lore:_Wind_of_Soltia-$version-x86_64.AppImage" "HeroesLore-$version-x86_64.AppImage" 2>/dev/null || true
ls -l "$out"/*.AppImage
