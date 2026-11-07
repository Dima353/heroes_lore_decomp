# Heroes Lore Wind of Soltia Port for Windows
### Build environment

1. Install Visual Studio 2022 (Community is enough) with the "Desktop development with C++" workload. It includes CMake and Ninja.
2. Install Git for Windows: [link](https://git-scm.com/download/win)
3. Optional, for embedded music: Python 3 and ffmpeg, see [soundfont.md](soundfont.md).

zlib, minizip and SDL2 are downloaded by CMake and linked statically; nothing else has to be installed.

### Build
1. Open "Developer Command Prompt for VS 2022".
2. Clone the repository:
```bat
git clone https://github.com/Dima353/heroes_lore_decomp.git
cd heroes_lore_decomp\cpp
```
3. Configure and build:
```bat
cmake --preset windows
cmake --build --preset windows
```
4. Create the package:
```bat
cd build-windows
cpack -C Release
```
5. The package will be located at `cpp/build-windows/HeroesLore-<version>-win64.zip`.

### Install
1. Extract the package.
2. Place the game `.jar` next to `heroes_lore.exe`, or drop it onto the window.
3. Optional: download GeneralUser GS ([link](https://schristiancollins.com/generaluser.php)) and place `GeneralUser-GS.sf2` next to `heroes_lore.exe`.
4. Run `heroes_lore.exe`.

### Notes
- F11 or Alt+Enter toggles fullscreen.
- Saves, settings and `heroes_lore.log`: `%APPDATA%\HeroesLore\HeroesLore`.
