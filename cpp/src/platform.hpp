#pragma once

// Build targets. Consoles share the fixed-screen, no-snapshot, data-folder behaviour;
// HEROES_LORE_SWITCH / HEROES_LORE_VITA select what only one of them does.
#if defined(__SWITCH__) || defined(SWITCH) || defined(HEROES_LORE_SWITCH_BUILD)
#define HEROES_LORE_SWITCH 1
#else
#define HEROES_LORE_SWITCH 0
#endif
#if defined(__vita__) || defined(HEROES_LORE_VITA_BUILD)
#define HEROES_LORE_VITA 1
#else
#define HEROES_LORE_VITA 0
#endif
#if defined(__PSP__) || defined(HEROES_LORE_PSP_BUILD)
#define HEROES_LORE_PSP 1
#else
#define HEROES_LORE_PSP 0
#endif
#if defined(__3DS__) || defined(HEROES_LORE_3DS_BUILD)
#define HEROES_LORE_3DS 1
#else
#define HEROES_LORE_3DS 0
#endif
#define HEROES_LORE_CONSOLE (HEROES_LORE_SWITCH || HEROES_LORE_VITA || HEROES_LORE_PSP || HEROES_LORE_3DS)
// Weak handhelds: 30 FPS render cap, 22050 Hz float synth, small voice pools, no external .sf2.
#define HEROES_LORE_LOW_POWER (HEROES_LORE_PSP || HEROES_LORE_3DS)

// Where a console keeps the JAR, saves, settings, the log and an optional .sf2.
#if HEROES_LORE_SWITCH
#define HEROES_LORE_DATA_DIR "sdmc:/switch/heroeslore"
#elif HEROES_LORE_PSP
#define HEROES_LORE_DATA_DIR "."
#elif HEROES_LORE_3DS
#define HEROES_LORE_DATA_DIR "sdmc:/3ds/heroeslore"
#elif HEROES_LORE_VITA
#define HEROES_LORE_DATA_DIR "ux0:data/heroeslore"
#endif
