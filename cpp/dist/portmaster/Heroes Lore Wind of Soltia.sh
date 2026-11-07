#!/bin/bash
# PortMaster launcher for the Heroes Lore: Wind of Soltia port.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="/$directory/ports/heroeslore"
cd "$GAMEDIR"
> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

# Saves, settings and heroes_lore.log stay inside the port folder (conf/HeroesLore/HeroesLore).
export XDG_DATA_HOME="$GAMEDIR/conf"
mkdir -p "$XDG_DATA_HOME"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
export LD_LIBRARY_PATH="$GAMEDIR/libs.${DEVICE_ARCH}:$LD_LIBRARY_PATH"

if ! ls "$GAMEDIR"/*.jar >/dev/null 2>&1; then
  echo "Heroes Lore Wind of Soltia: copy the game's .jar (J2ME) into ports/heroeslore and start again."
  sleep 5
  pm_finish
  exit 1
fi

# gptokeyb only provides the exit hotkey: the game reads the pad itself.
$GPTOKEYB "heroes_lore.${DEVICE_ARCH}" &
pm_platform_helper "$GAMEDIR/heroes_lore.${DEVICE_ARCH}"
./heroes_lore.${DEVICE_ARCH}

pm_finish
