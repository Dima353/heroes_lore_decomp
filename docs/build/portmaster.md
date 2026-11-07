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
sudo apt install -y git make cmake python3 build-essential binfmt-support libarchive-tools qemu-user-static gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
```
2. Create the arm64 Ubuntu 20.04 sysroot:
```bash
cd ~
wget https://cloud-images.ubuntu.com/releases/focal/release/ubuntu-20.04-server-cloudimg-arm64-wsl.rootfs.tar.gz
mkdir arm64-sysroot
sudo bsdtar -xpf ubuntu-20.04-server-cloudimg-arm64-wsl.rootfs.tar.gz -C arm64-sysroot
sudo cp /usr/bin/qemu-aarch64-static arm64-sysroot/usr/bin
sudo cp /etc/resolv.conf arm64-sysroot/etc/resolv.conf
```
3. Install SDL2 into the sysroot:
```bash
sudo mount -o bind /dev arm64-sysroot/dev
sudo chroot arm64-sysroot qemu-aarch64-static /bin/bash
apt update
apt install -y libsdl2-dev
exit
sudo umount arm64-sysroot/dev
echo 'export HEROES_LORE_SYSROOT=$HOME/arm64-sysroot' >> ~/.bashrc
source ~/.bashrc
```

Optional, for embedded music: [soundfont.md](soundfont.md).

### Build
1. Clone the repository:
```bash
git clone https://github.com/Dima353/heroes_lore_decomp.git
cd heroes_lore_decomp/cpp
```
2. Configure, build and package:
```bash
cmake --preset portmaster
cmake --build --preset portmaster -j8
python3 ../tools/package_portmaster.py ~/heroeslore-portmaster/heroes_lore heroeslore-portmaster.zip
```
3. The package will be located at `cpp/heroeslore-portmaster.zip`.

### Install
1. Extract the package to `ports/`.
2. Copy the game `.jar` to `ports/heroeslore/`.
3. Optional: copy `GeneralUser-GS.sf2` ([link](https://schristiancollins.com/generaluser.php)) to `ports/heroeslore/`.
4. Run Heroes Lore Wind of Soltia from Ports.

### Notes
- Saves, settings and `heroes_lore.log`: `ports/heroeslore/conf/HeroesLore/HeroesLore`.
