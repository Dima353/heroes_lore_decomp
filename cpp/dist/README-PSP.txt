Heroes Lore Wind of Soltia - PlayStation Portable port

Needs custom firmware (ARK-4 or PRO/LME).
Copy PSP/GAME/HeroesLore to the memory card and put the game's .jar there as
Heroes_Lore.jar. To upgrade, replace only EBOOT.PBP.

30 FPS rendering; game logic keeps the original tick rate.
Music: TinySoundFont with an embedded GeneralUser GS subset (samples at most
22050 Hz). No external .sf2 is needed.
Options -> Music switches between it and the built-in synth.

Cross: attack / interact / confirm
Square: special attack / page text
Circle: calm guardian / cancel / skip dialogue
Triangle: wild guardian / shop mode / recipes in menus
Start: menu; Select: map; L/R: quick item / next item

no-audio.txt beside EBOOT.PBP disables audio on the next launch.
trace.txt beside EBOOT.PBP writes a detailed psp_trace.log (slower).
Saves, settings and heroes_lore.log stay beside EBOOT.PBP.
See SOUNDFONT-LICENSE.txt for the GeneralUser GS license.
