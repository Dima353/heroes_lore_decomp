# Heroes Lore Wind of Soltia Port for PortMaster
### Build environment

Ubuntu 22.04, native or in WSL2. The binary is cross-compiled for aarch64 against an Ubuntu 20.04 arm64 sysroot, so it needs only GLIBC 2.17 and the SDL2 of the device.

On Windows, install WSL2 from PowerShell and restart:
```PowerShell
wsl --install Ubuntu-22.04
```

1. Install the cross compiler and tools:
```bash
sudo apt update && sudo apt upgrade -y
sudo apt install -y git make cmake python3 build-essential binfmt-support qemu-user-static gcc-aarch64-linux-gnu g++-aarch64-linux-gnu wget
```
2. Clone the repository:
```bash
git clone https://github.com/Dima353/heroes_lore_decomp.git
cd heroes_lore_decomp
```
3. Create the arm64 Ubuntu 20.04 sysroot. The script downloads Ubuntu base 20.04.5 and SDL2 2.32.0 (checked by SHA-256), installs the SDL2 development packages and builds SDL2 2.32.0 inside the sysroot under qemu; this takes about 10 minutes:
```bash
sudo sh tools/make_portmaster_sysroot.sh ~/arm64-sysroot
echo 'export HEROES_LORE_SYSROOT=$HOME/arm64-sysroot' >> ~/.bashrc
source ~/.bashrc
```

Optional, for embedded music: [soundfont.md](soundfont.md).

### Build
1. Configure, build and package from `cpp/`:
```bash
cd cpp
cmake --preset portmaster
cmake --build --preset portmaster -j8
python3 ../tools/package_portmaster.py ~/heroeslore-portmaster/heroes_lore heroeslore-portmaster.zip
```
2. The package will be located at `cpp/heroeslore-portmaster.zip`.

### Install
1. Extract the package to `ports/`.
2. Copy the game `.jar` to `ports/heroeslore/`.
3. Optional: copy `GeneralUser-GS.sf2` ([link](https://schristiancollins.com/generaluser.php)) to `ports/heroeslore/`.
4. Run Heroes Lore Wind of Soltia from Ports.

### Notes
- Saves, settings and `heroes_lore.log`: `ports/heroeslore/conf/HeroesLore/HeroesLore`.
