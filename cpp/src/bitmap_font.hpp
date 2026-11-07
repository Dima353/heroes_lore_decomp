#pragma once

#include "sprite.hpp"

#include <cstdint>
#include <utility>
#include <string>
#include <vector>

// A glyph appended to the atlas for a codepoint outside the font's ASCII table.
struct BitmapExtraGlyph { std::uint32_t codepoint{}, left{}, width{}; };
struct BitmapFont {
    std::uint8_t height{};
    std::uint8_t baseline{};
    std::uint8_t space_width{};
    std::uint8_t spacing{};
    bool has_special_glyphs{};
    std::vector<std::uint16_t> glyph_offsets;
    DecodedImage atlas;
    std::uint32_t text_palette_rgb{};
    std::uint32_t shadow_palette_rgb{};
    bool has_shadow_palette{};
    std::vector<BitmapExtraGlyph> extra_glyphs; // port: native Cyrillic from a translated font
};

struct BitmapGlyphPlacement {
    std::uint32_t source_x{};
    std::uint32_t source_width{};
    int x{};
    int y{};
    std::size_t text_end{};
};

BitmapFont parse_bitmap_font(const std::vector<std::uint8_t>& bytes);
BitmapFont load_bitmap_font(const GameArchive& archive, const std::string& path);
// Appends `source` glyphs to `font`: each pair is (codepoint to add, ASCII code in `source`).
// Both fonts must already share colours (recolor first).
void add_bitmap_glyphs(BitmapFont& font, const BitmapFont& source,
                       const std::vector<std::pair<std::uint32_t, std::uint32_t>>& glyphs);
// Builds `added` from glyph `base` with a breve (Й) or a diaeresis (Ё, ё) above it, in the
// colour of the base glyph's top pixel; replaces an existing entry for `added`.
void add_marked_glyph(BitmapFont& font, std::uint32_t base, std::uint32_t added, bool diaeresis);
void recolor_bitmap_font(BitmapFont& font, std::uint32_t text_rgb,
                         std::uint32_t shadow_rgb = 0x000000);
std::vector<BitmapGlyphPlacement> layout_bitmap_text(
    const BitmapFont& font, const std::string& utf8, int x, int y,
    int max_width, int scale = 1);

