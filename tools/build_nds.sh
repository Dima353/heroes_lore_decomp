#!/bin/sh
# Nintendo DS build in WSL/Linux with devkitPro (nds-dev, nds-zlib); see docs/build/nds.md.
# make cannot handle spaces in paths, so the sources are copied to $HOME/heroeslore-nds first.
# Usage: tools/build_nds.sh [output-dir] [game.jar]  (default: builds/nds)
# With a game.jar it also writes heroes_lore_jar.nds, the JAR packed inside (see docs/build/nds.md).
set -eu
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}" DEVKITARM="${DEVKITARM:-/opt/devkitpro/devkitARM}"
export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
out="${1:-$root/builds/nds}"
jar=""
if [ $# -gt 1 ] && [ -n "$2" ]; then jar=$(realpath "$2"); fi
work="$HOME/heroeslore-nds"
mkdir -p "$work/src" "$work/nds"
cp -u "$root"/cpp/src/*.cpp "$root"/cpp/src/*.hpp "$root"/cpp/src/*.inc "$work/src/"
cp -ru "$root"/cpp/nds/. "$work/nds/"
mkdir -p "$work/src/third_party" && cp -u "$root"/cpp/src/third_party/*.h "$work/src/third_party/"
# Instrument bank from GeneralUser GS 2.0.3 (HEROES_LORE_SF2, next to the repository, or downloaded
# into $work); rebuilt when the SoundFont, the used-notes list or the script change. Without the
# SoundFont the music is silent.
sf2="${HEROES_LORE_SF2:-$root/../GeneralUser-GS.sf2}"
if [ ! -f "$sf2" ]; then
    sf2="$work/GeneralUser-GS.sf2"
    if [ ! -f "$sf2" ]; then
        echo "Downloading GeneralUser GS 2.0.3 (32 MB)" >&2
        if wget -q -O "$sf2.part" https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/97049183643d5fc5a9322a69c5b09efb667c6c3a/GeneralUser-GS.sf2 &&
           echo "9575028c7a1f589f5770fccc8cff2734566af40cd26ed836944e9a5152688cfe  $sf2.part" | sha256sum -c --quiet; then
            mv "$sf2.part" "$sf2"
        else
            rm -f "$sf2.part"
        fi
    fi
fi
bank="$work/nds/data/soundbank.bin"
mkdir -p "$work/nds/data"
if [ -f "$sf2" ]; then
    if [ ! -f "$bank" ] || [ "$(wc -c < "$bank")" -lt 64 ] || [ "$sf2" -nt "$bank" ] || [ "$root/tools/build_nds_soundbank.py" -nt "$bank" ] ||
       [ "$root/cpp/dist/soundfont/used-notes.json" -nt "$bank" ]; then
        python3 "$root/tools/build_nds_soundbank.py" --source "$sf2" --used "$root/cpp/dist/soundfont/used-notes.json" --output "$bank"
    fi
elif [ ! -f "$bank" ]; then
    echo "warning: $sf2 not found, building without music instruments" >&2
    python3 -c "import sys; open(sys.argv[1], 'wb').write(b'HLSB' + bytes(12))" "$bank"
fi
make -C "$work/nds" -j4
mkdir -p "$out"
cp "$work/nds/heroes_lore.nds" "$out/heroes_lore.nds"
if [ -n "$jar" ]; then
    rm -f "$work/nds/heroes_lore_jar.nds"
    cp "$jar" "$work/nds/game.jar" # make cannot handle spaces in paths
    make -C "$work/nds" jar GAME_JAR=game.jar
    cp "$work/nds/heroes_lore_jar.nds" "$out/heroes_lore_jar.nds"
    ls -l "$out/heroes_lore_jar.nds"
fi
"$DEVKITARM/bin/arm-none-eabi-size" "$work/nds/heroes_lore.elf"
ls -l "$out/heroes_lore.nds"
