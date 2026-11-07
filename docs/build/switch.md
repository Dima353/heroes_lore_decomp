# Heroes Lore Wind of Soltia Port for Nintendo Switch
### Build environment

Ubuntu 22.04, native or in WSL2. On Windows, install WSL2 from PowerShell and restart:
```PowerShell
wsl --install Ubuntu-22.04
```

1. Install the base tools:
```bash
sudo apt update && sudo apt upgrade -y
sudo apt install -y wget git make cmake ninja-build python3 build-essential
```
2. Install devkitPro pacman ([Getting Started](https://devkitpro.org/wiki/Getting_Started)):
```bash
wget https://apt.devkitpro.org/install-devkitpro-pacman
chmod +x ./install-devkitpro-pacman
sudo ./install-devkitpro-pacman
echo 'export DEVKITPRO=/opt/devkitpro' >> ~/.bashrc
source ~/.bashrc
```
3. Install the Switch toolchain, SDL2 and zlib (minizip is part of `switch-zlib`):
```bash
sudo dkp-pacman -S switch-dev switch-cmake switch-sdl2 switch-zlib
```

Optional, for embedded music: [soundfont.md](soundfont.md).
Optional icon: a 256x256 JPEG at `cpp/assets/icon.jpg`; without it the default libnx icon is used.

### Build
1. Clone the repository:
```bash
git clone https://github.com/Dima353/heroes_lore_decomp.git
cd heroes_lore_decomp/cpp
```
2. Configure and build:
```bash
cmake --preset switch-wsl
cmake --build --preset switch-wsl
```
3. The program will be located at `~/heroeslore-build/heroes_lore.nro`.

### Install
1. Copy `heroes_lore.nro` to `sdmc:/switch/heroeslore/`.
2. Copy the game `.jar` to `sdmc:/switch/heroeslore/`.
3. Optional: copy `GeneralUser-GS.sf2` ([link](https://schristiancollins.com/generaluser.php)) to `sdmc:/switch/heroeslore/`.
4. Run Heroes Lore from the Homebrew Menu.

### Notes
- Saves, settings and `heroes_lore.log`: `sdmc:/switch/heroeslore/`.
- HD Rumble on Joy-Con and Pro Controller; it can be turned off in Options.
