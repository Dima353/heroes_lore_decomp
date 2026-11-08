# Heroes Lore Wind of Soltia Port for Nintendo DS
### Build environment

Ubuntu 22.04, native or in WSL2. On Windows, install WSL2 from PowerShell and restart:
```PowerShell
wsl --install Ubuntu-22.04
```

1. Install the base tools:
```bash
sudo apt update && sudo apt upgrade -y
sudo apt install -y wget git make python3
```
2. Install devkitPro pacman ([Getting Started](https://devkitpro.org/wiki/Getting_Started)) and the DS toolchain:
```bash
wget https://apt.devkitpro.org/install-devkitpro-pacman
chmod +x ./install-devkitpro-pacman
sudo ./install-devkitpro-pacman
sudo dkp-pacman -S nds-dev nds-zlib
echo 'export DEVKITPRO=/opt/devkitpro' >> ~/.bashrc
echo 'export DEVKITARM=/opt/devkitpro/devkitARM' >> ~/.bashrc
source ~/.bashrc
```
`nds-dev` brings devkitARM, libnds, calico, libfat and ndstool; `nds-zlib` brings zlib and minizip.

The DS build does not use SDL2 or CMake. It has its own front end in `cpp/nds/` on top of the shared game code in `cpp/src/`.

### Build `.nds`
1. Clone the repository:
```bash
git clone https://github.com/Dima353/heroes_lore_decomp.git
cd heroes_lore_decomp
```
2. Build:
```bash
sh tools/build_nds.sh
```
`make` cannot handle spaces in paths, so the script copies the sources to `~/heroeslore-nds` and builds there.

3. The program will be located at `builds/nds/heroes_lore.nds`. Pass a folder to put it elsewhere: `sh tools/build_nds.sh ~/out`.

### Build `.nds` with the game `.jar` inside
By default the `.nds` contains no game files and reads the `.jar` from the SD card. With a `.jar` as the second argument the script also writes `heroes_lore_jar.nds`, which carries the `.jar` linked into the program, so nothing else has to be copied to the SD card. Such a file contains the game: keep it for your own console and do not share it.
```bash
sh tools/build_nds.sh builds/nds /path/to/Heroes_Lore.jar
```
The result is `builds/nds/heroes_lore_jar.nds` next to the plain `heroes_lore.nds`. The packed `.jar` is loaded into RAM with the program and read in place, so it works with any loader and uses no more memory than the plain build.

### Music instruments
Music plays on the DS sound hardware from a small instrument bank, built by `tools/build_nds_soundbank.py` from GeneralUser GS 2.0.3 and the note list in `cpp/dist/soundfont/used-notes.json`. No game files are needed.

The script looks for the SoundFont in this order:
1. The path in `HEROES_LORE_SF2`.
2. `GeneralUser-GS.sf2` in the folder above the repository.
3. `~/heroeslore-nds/GeneralUser-GS.sf2`. If the file is missing, the script downloads it from the [official repository](https://github.com/mrbumpy409/GeneralUser-GS) (32 MB, checked by SHA-256).

If no SoundFont can be found, the build finishes with an empty bank and the music is silent; sound effects still play. The bank is rebuilt when the SoundFont, the note list or the script changes.

### Install
1. Copy `heroes_lore.nds` anywhere on the SD card of a flashcart, or of a DSi or 3DS running TWiLight Menu++.
2. Copy the game `.jar` as `Heroes_Lore.jar` to the `heroeslore/` folder next to the `.nds`. Not needed for `heroes_lore_jar.nds`. `/heroeslore/` and the SD card root also work.
3. Start the `.nds` from the loader.

The program looks for the `.jar` in this order: `heroeslore/` next to the `.nds`, the folder of the `.nds`, `/heroeslore/`, the SD card root, the `.jar` packed in the `.nds`. A `.jar` on the SD card always takes precedence over the packed one.

The `.jar` is read into RAM at start; the English release is preferred. The loader must give the program SD access (DLDI on flashcarts, built in on DSi).

### Notes
- Targets the original Nintendo DS (4 MB RAM, ARM9 at 67 MHz) at 60 FPS.
- The top screen shows the field (256x192). Boss arenas taller than the screen extend onto the bottom screen.
- The bottom screen holds the minimap or location plate, dialogue with the speaker's face, the HP/SP row and five touch buttons (guardian skills, menu, quick item, next item). The game menu, shops, services and the title menus are drawn there too, with touch.
- Keys: D-pad move, A talk / attack, X skill, B / Y guardian (calm / wild), L use item, R change item, START menu; L / R switch the Options tabs.
- Russian is built in and selected in Options, as on the other platforms.
- Saves (`hero<class>.hls`, `autosave.hls`), `settings.hlc`, `volume.cfg` and `heroes_lore_nds.log` are written to the `heroeslore/` folder next to the `.nds`, which is created on the first start. If the loader does not pass the path of the `.nds`, they go next to the `.jar` (`/heroeslore/` for the packed one). Saves of earlier builds stay next to their `.jar`; move them to the new folder if the `.nds` is elsewhere.
