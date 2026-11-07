#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Localized resources built into the port (fonts/, lang/ and fr-FR/ entries of a translated release).
struct LanguagePackFile { const char* name; const std::uint8_t* data; std::size_t size; };
struct LanguagePack { const LanguagePackFile* files; std::size_t count; };

const LanguagePack& russian_language_pack();
const LanguagePackFile* find_language_pack_file(const LanguagePack& pack, const std::string& name);

// The Russian fonts place Cyrillic glyphs on ASCII codes (A->А, e->а, '>'->ы, '\'->ь, ...).
// Converts UTF-8 Russian text to those codes so port-only strings render with the Russian font.
std::string encode_russian_font_text(const std::string& utf8);
// (Cyrillic codepoint, ASCII code of its glyph in the Russian fonts) for building native-Cyrillic fonts.
std::vector<std::pair<std::uint32_t, std::uint32_t>> russian_font_glyphs();
// Inverse of the font encoding: the Russian language bank's ASCII letters (and : ' * < \ > ^ { })
// become real Cyrillic UTF-8, so Latin strings can share the same native-Cyrillic font.
std::string decode_russian_font_text(const std::string& text);
