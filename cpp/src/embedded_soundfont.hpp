#pragma once
#include <cstddef>

// The compressed minimal SoundFont built into the program (tools/build_psp_soundfont.py, zlib
// stream; CMake HEROES_LORE_SOUNDFONT_ZLIB). False when this build carries none.
bool embedded_soundfont(const void** data, std::size_t* size);
