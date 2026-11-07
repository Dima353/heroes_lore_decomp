# Heroes Lore Wind of Soltia Port for PlayStation Portable
### Build environment

The PSP build runs in Docker with a pinned `pspdev/pspdev` image (`cpp/cmake/Dockerfile.psp`), which contains PSPSDK, CMake and SDL2.

1. Install Git for Windows: [link](https://git-scm.com/download/win)
2. Install Docker Desktop with the WSL2 backend (Linux containers): [link](https://www.docker.com/products/docker-desktop/)
3. Build the embedded SoundFont: [soundfont.md](soundfont.md). The PSP plays music only through it; without it the built-in synth is used.

On Linux, install Docker and run the container step from `tools/build_psp.ps1` by hand (see below).

### Build
1. Clone the repository:
```powershell
git clone https://github.com/Dima353/heroes_lore_decomp.git
cd heroes_lore_decomp
```
2. Build:
```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build_psp.ps1
```
On Linux:
```bash
docker build -t heroeslore-psp -f cpp/cmake/Dockerfile.psp cpp/cmake
docker run --rm -v "$PWD:/src" -w /src heroeslore-psp sh tools/build_psp_container.sh
```
3. The program will be located at `builds/psp/PSP/GAME/HeroesLore/EBOOT.PBP`.

Optional XMB icon: a 144x80 PNG at `cpp/assets/psp/icon0.png`.

### Install
1. The PSP needs custom firmware (ARK-4 or PRO/LME).
2. Copy `PSP/GAME/HeroesLore/` to the memory card.
3. Copy the game `.jar` into `PSP/GAME/HeroesLore/` as `Heroes_Lore.jar`.
4. Run Heroes Lore Wind of Soltia from the Game menu.

### Notes
- Rendering is capped at 30 FPS; game logic keeps the original tick rate.
- Saves, settings and `heroes_lore.log` stay beside the EBOOT.
- `no-audio.txt` beside the EBOOT disables audio; `trace.txt` writes `psp_trace.log`.
