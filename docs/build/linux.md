# Heroes Lore Wind of Soltia Port for Linux and Steam Deck
### Build environment

Ubuntu 22.04, native or in WSL2. The package targets GLIBC 2.34 and runs on SteamOS; a newer distribution produces a binary that needs its newer GLIBC.

On Windows, install WSL2 from PowerShell and restart:
```PowerShell
wsl --install Ubuntu-22.04
```

Install the tools and SDL2:
```bash
sudo apt update && sudo apt upgrade -y
sudo apt install -y git build-essential cmake ninja-build pkg-config libsdl2-dev
```
zlib and minizip are downloaded by CMake and linked statically. SDL2 is linked from the system and loaded from the device at run time.

Optional, for embedded music: [soundfont.md](soundfont.md).

### Build
1. Clone the repository:
```bash
git clone https://github.com/Dima353/heroes_lore_decomp.git
cd heroes_lore_decomp/cpp
```
2. Configure and build:
```bash
cmake --preset linux
cmake --build --preset linux
```
3. Create the package:
```bash
cd ~/heroeslore-linux
cpack
```
4. The package will be located at `~/heroeslore-linux/HeroesLore-<version>-Linux.tar.gz`.

### AppImage
A single-file build with SDL2 bundled. From the repository root, after the build above:
```bash
sh tools/package_appimage.sh
```
The script downloads a pinned `linuxdeploy` release into the build folder on first use. The AppImage will be located at `~/heroeslore-linux/HeroesLore-<version>-x86_64.AppImage`.
Optional icon: a 256x256 PNG at `cpp/assets/linux/heroes_lore.png`; otherwise `cpp/dist/linux/heroes_lore.png` is used.

### Install on Linux
1. Extract the package, or take the AppImage.
2. Place the game `.jar` next to `heroes_lore` or the `.AppImage`.
3. Optional: place `GeneralUser-GS.sf2` ([link](https://schristiancollins.com/generaluser.php)) in the same folder.
4. Run `./heroes_lore`, or `chmod +x` the `.AppImage` and run it.

### Install on Steam Deck
1. Switch to Desktop Mode.
2. Copy the `.AppImage` to `~/Games/HeroesLore` (or extract the `tar.gz` package there).
3. Place the game `.jar` and, optionally, `GeneralUser-GS.sf2` in `~/Games/HeroesLore`.
4. Make the program executable:
```bash
chmod +x ~/Games/HeroesLore/*.AppImage
```
5. Steam, Games, Add a Non-Steam Game to My Library, Browse, select the `.AppImage` (or `heroes_lore`).
6. Return to Gaming Mode and run Heroes Lore Wind of Soltia.

### Notes
- Saves, settings and `heroes_lore.log`: `~/.local/share/HeroesLore/HeroesLore`.
