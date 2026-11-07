# Heroes Lore Wind of Soltia Port for PlayStation Vita
### Build environment

Ubuntu 22.04, native or in WSL2. On Windows, install WSL2 from PowerShell and restart:
```PowerShell
wsl --install Ubuntu-22.04
```

1. Install the base tools:
```bash
sudo apt update && sudo apt upgrade -y
sudo apt install -y wget curl git make cmake python3 python-is-python3 bzip2 xz-utils pkgconf build-essential libtool
```
2. Install VitaSDK with all packages (SDL2 with vitaGL and their dependencies) through vdpm. zlib and minizip are downloaded by CMake and linked statically:
```bash
git clone https://github.com/vitasdk/vdpm
cd vdpm
./bootstrap-vitasdk.sh
echo 'export VITASDK=/usr/local/vitasdk' >> ~/.bashrc
echo 'export PATH=$VITASDK/bin:$PATH' >> ~/.bashrc
source ~/.bashrc
./install-all.sh
cd ..
```

Optional, for embedded music: [soundfont.md](soundfont.md).
Optional LiveArea art: `cpp/assets/vita/sce_sys/icon0.png` and `cpp/assets/vita/sce_sys/livearea/contents/{bg.png,startup.png,template.xml}`.

### Build
1. Clone the repository:
```bash
git clone https://github.com/Dima353/heroes_lore_decomp.git
cd heroes_lore_decomp/cpp
```
2. Configure and build:
```bash
cmake --preset vita
cmake --build --preset vita -j4
```
3. The package will be located at `~/heroeslore-vita/heroes_lore.vpk`.

### Install
1. Install `ur0:data/libshacccg.suprx` (for example with ShaRKBR33D).
2. Install `heroes_lore.vpk` with VitaShell.
3. Copy the game `.jar` to `ux0:data/heroeslore/`.
4. Optional: copy `GeneralUser-GS.sf2` ([link](https://schristiancollins.com/generaluser.php)) to `ux0:data/heroeslore/`.
5. Run Heroes Lore Wind of Soltia from LiveArea.

### Notes
- Saves, settings and `heroes_lore.log`: `ux0:data/heroeslore/`.
- Overclocking is recommended.
