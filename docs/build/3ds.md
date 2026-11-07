# Heroes Lore Wind of Soltia Port for Nintendo 3DS
### Build environment

Ubuntu 22.04, native or in WSL2. On Windows, install WSL2 from PowerShell and restart:
```PowerShell
wsl --install Ubuntu-22.04
```

1. Install the base tools:
```bash
sudo apt update && sudo apt upgrade -y
sudo apt install -y wget git make cmake python3 build-essential
```
2. Install devkitPro pacman ([Getting Started](https://devkitpro.org/wiki/Getting_Started)) and the 3DS toolchain:
```bash
wget https://apt.devkitpro.org/install-devkitpro-pacman
chmod +x ./install-devkitpro-pacman
sudo ./install-devkitpro-pacman
sudo dkp-pacman -S 3ds-dev 3ds-cmake 3ds-pkg-config citro2d
echo 'export DEVKITPRO=/opt/devkitpro' >> ~/.bashrc
echo 'export DEVKITARM=/opt/devkitpro/devkitARM' >> ~/.bashrc
source ~/.bashrc
```
devkitPro has no SDL2 package for 3DS; the build downloads SDL2 2.30.9, zlib and minizip and links them statically.

3. Build the embedded SoundFont: [soundfont.md](soundfont.md). The 3DS plays music only through it (`.sf2` files on the SD card are ignored); without it the built-in synth is used.

The icon and banner are in `cpp/dist/3ds/`; files in `cpp/assets/3ds/` override them.

### Build `.3dsx`
1. Clone the repository:
```bash
git clone https://github.com/Dima353/heroes_lore_decomp.git
cd heroes_lore_decomp/cpp
```
2. Configure and build:
```bash
cmake --preset 3ds
cmake --build --preset 3ds -j4
```
3. The program will be located at `~/heroeslore-3ds/heroes_lore.3dsx`. `tools/build_3ds.sh` runs both commands and copies the result to `builds/3ds/`.

### `.cia` for the HOME Menu
See [3ds-cia.md](3ds-cia.md).

### Install
1. Copy `heroes_lore.3dsx` and the game `.jar` to `sdmc:/3ds/heroeslore/`.
2. Run Heroes Lore Wind of Soltia from the Homebrew Launcher.

### Notes
- The game uses the top screen (400x240) with stereoscopic 3D. The bottom screen holds the minimap, the enemy panel, the HUD strip and the game menu.
- Touch: the five bottom buttons (guardian skills, menu, quick item, next item), the HP/SP row (menu) and the minimap (full map). In menus a tap on a tab opens it, a tap on a row picks it and a second tap confirms; the bottom corners act as A and B; a tap advances dialogue.
- Rendering goes through citro2d at 30 FPS. `-DHEROES_LORE_3DS_GPU=OFF` builds the SDL software renderer instead (no 3D).
- Saves, settings and `heroes_lore.log`: `sdmc:/3ds/heroeslore/`, shared by the `.3dsx` and the `.cia`.
