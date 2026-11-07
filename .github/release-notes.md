Native port of the J2ME action RPG Heroes Lore: Wind of Soltia, rebuilt from the original bytecode in C++17 with SDL2.

**The game is not included.** Every package needs your own copy of the original J2ME `.jar` (the English release is preferred). Russian is selected in Options.

### Downloads

| Platform | File | Install |
|--|--|--|
| Windows x64 | `HeroesLore-*-Windows-x64.zip` | Extract, put the `.jar` next to `heroes_lore.exe` |
| Linux x64, Steam Deck | `HeroesLore-*-Linux-x86_64.AppImage` | `chmod +x`, put the `.jar` next to the AppImage |
| Linux x64 | `HeroesLore-*-Linux-x64.tar.gz` | Extract, put the `.jar` next to `heroes_lore` |
| Nintendo Switch | `HeroesLore-*-Switch.zip` | Extract to the SD card root, put the `.jar` in `switch/heroeslore/` |
| Nintendo 3DS | `HeroesLore-*-3DS.zip` | Extract to the SD card root, put the `.jar` in `3ds/heroeslore/` |
| PlayStation Vita | `HeroesLore-*-PSVita.vpk` | Install with VitaShell, put the `.jar` in `ux0:data/heroeslore/`; needs `ur0:data/libshacccg.suprx` |
| PlayStation Portable | `HeroesLore-*-PSP.zip` | Extract to the memory card root, put the `.jar` in `PSP/GAME/HeroesLore/` as `Heroes_Lore.jar`; custom firmware |
| PortMaster | `HeroesLore-*-PortMaster.zip` | Extract to `ports/`, put the `.jar` in `ports/heroeslore/` |

`SHA256SUMS.txt` lists the checksums.

### Notes
- Music plays through an embedded GeneralUser GS subset. On Windows, Linux, Switch, Vita and PortMaster a full `GeneralUser-GS.sf2` next to the `.jar` is used instead.
- 3DS and PSP render at 30 FPS; game logic keeps the original tick rate on every platform.
- PlayStation Vita: overclocking is recommended.
- A 3DS `.cia` is not provided; see [docs/build/3ds-cia.md](https://github.com/Dima353/heroes_lore_decomp/blob/main/docs/build/3ds-cia.md) to build one.
- Build instructions: [README](https://github.com/Dima353/heroes_lore_decomp#building-from-source).
