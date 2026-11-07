#!/bin/sh
# Nintendo 3DS build in WSL/Linux with devkitPro (see docs/build/3ds.md).
# Usage: tools/build_3ds.sh [game.jar]  (the JAR is packed into the .cia only; see docs/build/3ds-cia.md)
set -eu
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}" DEVKITARM="${DEVKITARM:-/opt/devkitpro/devkitARM}"
jar=""
if [ $# -gt 0 ]; then jar=$(realpath "$1"); fi
cd "$(dirname "$0")/../cpp"
cmake --preset 3ds -DHEROES_LORE_3DS_JAR="$jar"
cmake --build --preset 3ds -j4
mkdir -p ../builds/3ds/3ds/heroeslore
cp "$HOME/heroeslore-3ds/heroes_lore.3dsx" ../builds/3ds/3ds/heroeslore/heroes_lore.3dsx
if [ -f "$HOME/heroeslore-3ds/heroes_lore.cia" ]; then cp "$HOME/heroeslore-3ds/heroes_lore.cia" ../builds/3ds/heroes_lore.cia; fi
