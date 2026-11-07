#include "language_pack.hpp"

#include <array>
#include <cstring>

#include "language_pack_ru.inc"

const LanguagePack& russian_language_pack() {
    static const LanguagePack pack{ru_files, sizeof(ru_files) / sizeof(ru_files[0])};
    return pack;
}

const LanguagePackFile* find_language_pack_file(const LanguagePack& pack, const std::string& name) {
    for (std::size_t i = 0; i < pack.count; ++i)
        if (name == pack.files[i].name) return &pack.files[i];
    return nullptr;
}

namespace {
void append_utf8(std::string& out, std::uint32_t c) {
    if (c < 0x80) out += char(c);
    else { out += char(0xc0 | (c >> 6)); out += char(0x80 | (c & 0x3f)); }
}
}
std::vector<std::pair<std::uint32_t, std::uint32_t>> russian_font_glyphs() {
    std::vector<std::pair<std::uint32_t, std::uint32_t>> glyphs;
    const auto add = [&](std::uint32_t c) {
        std::string utf8;
        append_utf8(utf8, c);
        glyphs.emplace_back(c, std::uint32_t(static_cast<unsigned char>(encode_russian_font_text(utf8)[0])));
    };
    for (std::uint32_t c = 0x410; c <= 0x44f; ++c) add(c);
    add(0x401); add(0x451);
    return glyphs;
}
std::string decode_russian_font_text(const std::string& text) {
    static const auto table = [] {
        std::array<std::uint32_t, 128> decode{};
        for (const auto& glyph : russian_font_glyphs())
            if (!decode[glyph.second] && glyph.first != 0x419 && glyph.first != 0x42a &&
                glyph.first != 0x44a && glyph.first != 0x401 && glyph.first != 0x451)
                decode[glyph.second] = glyph.first; // shared glyphs decode to И, Ь/ь, Е/е; 'n' is й
        return decode;
    }();
    std::string out;
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 128 && table[c]) append_utf8(out, table[c]);
        else out += ch;
    }
    return out;
}
std::string encode_russian_font_text(const std::string& utf8) {
    // Glyph order of fonts/small.mf in the Russian release (codes 33..126).
    std::string out;
    for (std::size_t i = 0; i < utf8.size(); ++i) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        if (lead < 0x80 || i + 1 >= utf8.size()) { out += utf8[i]; continue; }
        const auto next = static_cast<unsigned char>(utf8[i + 1]);
        unsigned code = ((lead & 0x1f) << 6) | (next & 0x3f);
        ++i;
        if (code >= 0x410 && code <= 0x42f) {
            const unsigned index = code - 0x410;
            // Й (0x419) and Ъ (0x42a) have no glyph of their own.
            static const char table[32] = {'A','B','C','D','E','F','G','H','I','I','J','K','L','M','N','O',
                                           'P','Q','R','S','T','U','V','W','X','Y','a','Z','a','b','c','d'};
            out += table[index];
        } else if (code >= 0x430 && code <= 0x44f) {
            static const char table[32] = {'e','f','g','h','i','j','k','l','m','n','o','p','q','r','s','t',
                                           'u','v','w','x','y','z',':','\'','*','<','\\','>','\\','^','{','}'};
            out += table[code - 0x430];
        } else if (code == 0x401) out += 'F';   // Ё
        else if (code == 0x451) out += 'j';     // ё
        else out += '?';
    }
    return out;
}
