# Heroes Lore port source

C++17, SDL2, zlib and minizip. Builds use the CMake presets in `CMakePresets.json`.

### Presets

| Preset | Platform | Build folder |
|--|--|--|
| `windows` | Windows x64, Visual Studio 2022 | `cpp/build-windows` |
| `linux` | Linux x64 and Steam Deck | `~/heroeslore-linux` |
| `switch-wsl` | Nintendo Switch, devkitPro | `~/heroeslore-build` |
| `vita` | PlayStation Vita, VitaSDK | `~/heroeslore-vita` |
| `3ds` | Nintendo 3DS, devkitPro | `~/heroeslore-3ds` |
| `portmaster` | ARM64 Linux handhelds | `~/heroeslore-portmaster` |
| `inspect` | tools and tests only, no SDL | `cpp/build-inspect` |

The PSP build has no preset; it runs through `tools/build_psp.ps1`. Step-by-step guides: [../docs/build](../docs/build).

### Programs

`heroes_lore` is the game.

`heroes_lore_inspect` prints maps, shop and item data from a JAR.

`heroes_lore_frame` saves an atlas frame as PNG.

### Command line

```bash
heroes_lore <game.jar> [map_id] [zone] [switch_bits_hex]
```

Without arguments the game looks for a JAR beside itself and opens the title menu. A map id starts a debug session directly on that map.

`src/language_pack_ru.inc` holds the Russian fan translation (fonts, language bank, localized images); `tools/make_language_pack.py <english.jar> <russian.jar>` regenerates it.

### Optional local files

| File | Purpose |
|--|--|
| `assets/psp/heroes_lore.sf2.zlib` | embedded SoundFont, see [../docs/build/soundfont.md](../docs/build/soundfont.md) |
| `assets/icon.jpg` | Switch icon, 256x256 |
| `assets/vita/sce_sys/` | Vita LiveArea art |
| `assets/psp/icon0.png` | PSP XMB icon, 144x80 |
| `assets/3ds/icon.png`, `banner.png`, `banner.wav` | 3DS icon and `.cia` banner |
