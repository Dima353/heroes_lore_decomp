#include "embedded_soundfont.hpp"

#if defined(HEROES_LORE_EMBEDDED_SOUNDFONT) && defined(_WIN32)
// MSVC has no .incbin: the stream is an RCDATA resource (cmake/embedded_soundfont.rc.in).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
bool embedded_soundfont(const void** data, std::size_t* size) {
    HRSRC resource = FindResourceW(nullptr, L"HEROES_LORE_SF2", MAKEINTRESOURCEW(10)); // RT_RCDATA
    if (!resource) return false;
    HGLOBAL loaded = LoadResource(nullptr, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    if (!bytes) return false;
    *data = bytes;
    *size = SizeofResource(nullptr, resource);
    return *size > 0;
}
#elif defined(HEROES_LORE_EMBEDDED_SOUNDFONT)
// GCC toolchains: cmake/embedded_soundfont.S.in places the stream between these symbols.
extern "C" const unsigned char heroes_lore_sf2_start[], heroes_lore_sf2_end[];
bool embedded_soundfont(const void** data, std::size_t* size) {
    *data = heroes_lore_sf2_start;
    *size = std::size_t(heroes_lore_sf2_end - heroes_lore_sf2_start);
    return *size > 0;
}
#else
bool embedded_soundfont(const void**, std::size_t*) { return false; }
#endif
