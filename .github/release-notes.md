Native port of the J2ME action RPG Heroes Lore: Wind of Soltia, rebuilt from the original bytecode in C++17 with SDL2.

**The game is not included.** Every package needs your own copy of the original J2ME `.jar` (the English release is preferred). Russian is selected in Options.

### What's new
- **Nintendo DS**: new platform with its own front end for the original DS (4 MB RAM) at 60 FPS. Boss arenas span both screens; HUD, dialogue and touch menus are on the bottom screen; music plays on the DS sound hardware.
- **3DS**: dialogue, Yes/No questions and messages moved to the bottom screen with the speaker's face; the title menus are on the bottom screen with touch, the logo with falling feathers stays on top; the original STAT plate on the MENU button while attribute points wait.
- Headings are drawn in the original large font with its outline; Russian headings no longer show hollow letters.
- Modern menus: the trading post keeps its frame for the quantity and confirmation step.

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
| Nintendo DS | `HeroesLore-*-NDS.zip` | Copy `heroes_lore.nds` to the SD card (flashcart, or TWiLight Menu++ on DSi / 3DS), put the `.jar` in `heroeslore/` next to it as `Heroes_Lore.jar` (saves go there too) |
| PortMaster | `HeroesLore-*-PortMaster.zip` | Extract to `ports/`, put the `.jar` in `ports/heroeslore/` |

`SHA256SUMS.txt` lists the checksums.

### Notes
- Music plays through an embedded GeneralUser GS subset. On Windows, Linux, Switch, Vita and PortMaster a full `GeneralUser-GS.sf2` next to the `.jar` is used instead.
- Nintendo DS: music comes from a small instrument bank made from GeneralUser GS; `.sf2` files are not used.
- 3DS and PSP render at 30 FPS; game logic keeps the original tick rate on every platform.
- PlayStation Vita: overclocking is recommended.
- A 3DS `.cia` is not provided; see [docs/build/3ds-cia.md](https://github.com/Dima353/heroes_lore_decomp/blob/main/docs/build/3ds-cia.md) to build one.
- Build instructions: [README](https://github.com/Dima353/heroes_lore_decomp#building-from-source).
