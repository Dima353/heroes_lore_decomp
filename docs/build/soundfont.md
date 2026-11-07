# Embedded SoundFont
### Purpose

The program can embed a compressed subset of GeneralUser GS: only the presets and notes used by the game's music, resampled to at most 22050 Hz. It is the only music source on PSP and 3DS. On other platforms a full `.sf2` beside the program takes precedence; without either, the built-in synthesizer plays.

The subset is generated before compiling and is not stored in the repository. The list of presets and notes the game uses is stored in `cpp/dist/soundfont/used-notes.json`, so the game `.jar` is not needed. CI builds generate the subset the same way.

### Full build
The simplest way: add `-DHEROES_LORE_FULL_BUILD=ON` to the configure command of any CMake preset, for example:
```bash
cmake --preset linux -DHEROES_LORE_FULL_BUILD=ON
```
When `cpp/assets/psp/heroes_lore.sf2.zlib` is missing, configuration downloads GeneralUser GS 2.0.3 (32 MB, checked by SHA-256) to `cpp/assets/soundfont/` and generates the subset there. Later builds of every platform reuse it. Python 3 and ffmpeg must be installed on the build host. The PSP build runs in a container without them: generate the subset once with a full build of another platform, or by hand as below.

### Requirements
- Python 3.
- ffmpeg (sample-rate conversion).
- `GeneralUser-GS.sf2` v2.0.3 from the [official repository](https://github.com/mrbumpy409/GeneralUser-GS).

Ubuntu or WSL:
```bash
sudo apt install -y python3 ffmpeg wget
```
Windows: install Python 3 from python.org and download an ffmpeg build; pass its path with `--ffmpeg`.

### Manual build
From the repository root:
```bash
wget -O GeneralUser-GS.sf2 https://raw.githubusercontent.com/mrbumpy409/GeneralUser-GS/97049183643d5fc5a9322a69c5b09efb667c6c3a/GeneralUser-GS.sf2
mkdir -p builds/soundfont cpp/assets/psp
python3 tools/build_psp_soundfont.py --source GeneralUser-GS.sf2 --used cpp/dist/soundfont/used-notes.json --ffmpeg "$(command -v ffmpeg)" --output builds/soundfont/heroes_lore.sf2
cp builds/soundfont/heroes_lore.sf2.zlib cpp/assets/psp/heroes_lore.sf2.zlib
```
The script also writes a report next to the output with the presets used and checksums.

CMake picks up `cpp/assets/psp/heroes_lore.sf2.zlib` automatically on every platform (cache variable `HEROES_LORE_SOUNDFONT_ZLIB`). Reconfigure an existing build folder after adding it.

### Updating the note list
`used-notes.json` is audited from the MIDI files of the game. To rebuild it from your own copies (repeat `--jar` for every release):
```bash
python3 tools/build_psp_soundfont.py --source GeneralUser-GS.sf2 --jar Heroes_Lore.jar --save-used cpp/dist/soundfont/used-notes.json --ffmpeg "$(command -v ffmpeg)" --output builds/soundfont/heroes_lore.sf2
```

### License
GeneralUser GS may be redistributed; its license is in `cpp/dist/SOUNDFONT-LICENSE.txt` and is copied into the packages.
