#!/bin/sh
set -eu
cmake -S cpp -B cpp/build-psp \
  -DCMAKE_TOOLCHAIN_FILE="$PSPDEV/psp/share/pspdev.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DHEROES_LORE_FETCH_DEPS=ON -DHEROES_LORE_FETCH_SDL=OFF
cmake --build cpp/build-psp --target heroes_lore -j4
mkdir -p builds/psp/PSP/GAME/HeroesLore
cp cpp/build-psp/EBOOT.PBP builds/psp/PSP/GAME/HeroesLore/EBOOT.PBP
cp cpp/dist/README-PSP.txt builds/psp/README.txt
cp cpp/dist/SOUNDFONT-LICENSE.txt builds/psp/SOUNDFONT-LICENSE.txt