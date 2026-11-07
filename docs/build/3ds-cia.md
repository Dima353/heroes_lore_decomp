# Heroes Lore Wind of Soltia Port for Nintendo 3DS: `.cia`
### Purpose

A `.cia` installs the port as a HOME Menu title with a banner and an icon. It is built locally only: the release packages and CI provide the `.3dsx`.

By default the `.cia` contains no game files and reads the `.jar` from the SD card, like the `.3dsx`. With the option `HEROES_LORE_3DS_JAR` it carries your game `.jar` in its romfs, so nothing has to be copied to the SD card. Such a file contains the game: keep it for your own console and do not share it. The `.3dsx` never contains the `.jar`.

### Build environment

1. Set up the 3DS build environment from [3ds.md](3ds.md) (WSL2 or Ubuntu 22.04, devkitPro, `3ds-dev`, `citro2d`).
2. Build `makerom` from source; the release binary needs glibc 2.38:
```bash
sudo apt install -y wget build-essential
wget https://github.com/3DSGuy/Project_CTR/archive/refs/tags/makerom-v0.19.0.tar.gz
tar xzf makerom-v0.19.0.tar.gz
cd Project_CTR-makerom-v0.19.0/makerom
make deps && make
sudo install -m755 bin/makerom /opt/devkitpro/tools/bin/
cd ../..
```
3. Install `bannertool` 1.2.0 (mirror of the archived original):
```bash
wget https://github.com/diasurgical/bannertool/releases/download/1.2.0/bannertool.zip
python3 -m zipfile -e bannertool.zip bannertool
sudo install -m755 bannertool/linux-x86_64/bannertool /opt/devkitpro/tools/bin/
```

### Art

The banner and icon are in `cpp/dist/3ds/`:

| File | Format |
|--|--|
| `banner.png` | 256x128 PNG, transparent background |
| `banner.wav` | banner sound, up to 3 s (silence when missing) |
| `icon.png` | 48x48 PNG, also used by the `.3dsx` |

To use your own art, put the same files in `cpp/assets/3ds/` (not tracked in git); a `banner.png` there switches all three to that folder.

### Build
1. Configure and build from `cpp/`.
Without the game inside:
```bash
cmake --preset 3ds -DHEROES_LORE_3DS_JAR=
cmake --build --preset 3ds -j4
```
With the game `.jar` inside:
```bash
cmake --preset 3ds -DHEROES_LORE_3DS_JAR=/path/to/Heroes_Lore.jar
cmake --build --preset 3ds -j4
```
Full build with music and the game inside (about 9.7 MB; see [soundfont.md](soundfont.md)):
```bash
cmake --preset 3ds -DHEROES_LORE_FULL_BUILD=ON -DHEROES_LORE_3DS_JAR=/path/to/Heroes_Lore.jar
cmake --build --preset 3ds -j4
```
The configure step prints `3DS .cia skipped` when `makerom` or `bannertool` is missing.
`tools/build_3ds.sh [game.jar]` does the same and copies the results to `builds/3ds/`.
2. The package will be located at `~/heroeslore-3ds/heroes_lore.cia`, next to `heroes_lore.3dsx`.

### Install
1. Copy `heroes_lore.cia` to the SD card and install it with FBI.
2. If the `.cia` was built without the `.jar`, copy the game `.jar` to `sdmc:/3ds/heroeslore/`. A `.jar` there always takes precedence over the packed one.
3. Start Heroes Lore Wind of Soltia from the HOME Menu.

### Notes
- Saves, settings and `heroes_lore.log`: `sdmc:/3ds/heroeslore/`, shared with the `.3dsx`.
