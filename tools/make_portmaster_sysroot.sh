#!/bin/sh
# arm64 Ubuntu 20.04 sysroot for the PortMaster build (see docs/build/portmaster.md).
# Ubuntu base 20.04.5 plus the SDL2 development packages, and SDL2 2.32.0 built inside the
# sysroot (focal ships 2.0.10, too old for the port) into /usr/local. Runs as root on an x86_64
# Ubuntu host with qemu-user-static; building SDL2 under emulation takes several minutes.
# Usage: sudo sh tools/make_portmaster_sysroot.sh <sysroot-dir>
set -eu
dest=$1
base=ubuntu-base-20.04.5-base-arm64.tar.gz
base_sha=f9b999afb4c4b10193087ea8c11be36d688f19e609b05179b571f29357954b52
sdl=SDL2-2.32.0.tar.gz
sdl_sha=f5c2b52498785858f3de1e2996eba3c1b805d08fe168a47ea527c7fc339072d0
work=$(mktemp -d)
wget -q -O "$work/$base" "https://cdimage.ubuntu.com/ubuntu-base/releases/20.04/release/$base"
echo "$base_sha  $work/$base" | sha256sum -c
wget -q -O "$work/$sdl" "https://github.com/libsdl-org/SDL/releases/download/release-2.32.0/$sdl"
echo "$sdl_sha  $work/$sdl" | sha256sum -c
mkdir -p "$dest"
tar -xzf "$work/$base" -C "$dest"
cp /usr/bin/qemu-aarch64-static "$dest/usr/bin/"
cp /etc/resolv.conf "$dest/etc/resolv.conf"
cp "$work/$sdl" "$dest/tmp/"
rm -rf "$work"
mount -o bind /dev "$dest/dev"
trap 'umount "$dest/dev"' EXIT
chroot "$dest" qemu-aarch64-static /bin/bash -c "set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y -q libsdl2-dev build-essential cmake
cd /tmp && tar xzf $sdl
cmake -S ${sdl%.tar.gz} -B sdl-build -DCMAKE_BUILD_TYPE=Release -DSDL_STATIC=OFF -DSDL_TEST=OFF
cmake --build sdl-build -j\$(nproc)
cmake --install sdl-build
rm -rf sdl-build ${sdl%.tar.gz} $sdl"
echo "sysroot ready: $dest"
