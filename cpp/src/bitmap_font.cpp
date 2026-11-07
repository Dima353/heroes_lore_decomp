#include "bitmap_font.hpp"

#include "map.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>

namespace {
std::uint16_t read_u16_be(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 2)
        throw std::runtime_error("truncated bitmap-font u16");
    return static_cast<std::uint16_t>((std::uint16_t(bytes[offset]) << 8) |
                                      bytes[offset + 1]);
}

std::uint32_t read_rgb(const std::vector<std::uint8_t>& png, std::uint32_t offset) {
    if (offset > png.size() || png.size() - offset < 3)
        throw std::runtime_error("font palette offset outside PNG");
    return (std::uint32_t(png[offset]) << 16) |
           (std::uint32_t(png[offset + 1]) << 8) | png[offset + 2];
}

void replace_rgb(DecodedImage& image, std::uint32_t old_rgb, std::uint32_t new_rgb) {
    for (std::size_t i = 0; i + 3 < image.rgba.size(); i += 4) {
        const auto rgb = (std::uint32_t(image.rgba[i]) << 16) |
                         (std::uint32_t(image.rgba[i + 1]) << 8) | image.rgba[i + 2];
        if (rgb != old_rgb) continue;
        image.rgba[i] = static_cast<std::uint8_t>(new_rgb >> 16);
        image.rgba[i + 1] = static_cast<std::uint8_t>(new_rgb >> 8);
        image.rgba[i + 2] = static_cast<std::uint8_t>(new_rgb);
    }
}

// az.a(char): Latin-1 letters fold to their base glyph, other characters 128..255 draw as '.',
// anything beyond Latin-1 has no glyph and takes no space (returns 0). Typographic quotes keep
// the port's ASCII substitutes.
std::uint32_t glyph_codepoint(std::uint32_t c, const BitmapFont& font) {
    if (c == 0x2019) return '\'';
    if (c == 0x201c || c == 0x201d) return '"';
    if (c >= 256) return 0;
    if (c < 128) return c;
    if (c >= 192 && c < 223) c += 32; // capitals use the lower-case table
    std::uint32_t mapped = '.';
    if (c >= 224 && c <= 229) mapped = 'a';
    else if (c >= 232 && c <= 235) mapped = 'e';
    else if (c >= 236 && c <= 239) mapped = 'i';
    else if (c == 241) mapped = 'n';
    else if (c >= 242 && c <= 246) mapped = 'o';
    else if (c >= 249 && c <= 252) mapped = 'u';
    else if (font.has_special_glyphs && (c == 223 || c == 161 || c == 191)) mapped = c == 223 ? 127 : c == 161 ? 129 : 130;
    return mapped;
}

// Atlas span of a character; false when it takes no space.
bool glyph_span(std::uint32_t c, const BitmapFont& font, std::uint32_t& left, int& width) {
    for (const auto& extra : font.extra_glyphs)
        if (extra.codepoint == c) { left = extra.left; width = int(extra.width); return true; }
    c = glyph_codepoint(c, font);
    if (!c) return false;
    if (c < 33 || std::size_t(c - 33 + 1) >= font.glyph_offsets.size()) c = '.';
    const auto g = std::size_t(c - 33);
    left = font.glyph_offsets[g];
    width = int(font.glyph_offsets[g + 1]) - int(left);
    return true;
}

std::uint32_t next_codepoint(const std::string& text, std::size_t& offset) {
    const auto first = static_cast<std::uint8_t>(text[offset++]);
    if (first < 0x80) return first;
    unsigned continuation = first >= 0xf0 ? 3 : (first >= 0xe0 ? 2 : 1);
    if (offset + continuation > text.size()) return '?';
    std::uint32_t codepoint = first & (continuation == 1 ? 0x1f :
                                      continuation == 2 ? 0x0f : 0x07);
    for (unsigned i = 0; i < continuation; ++i) {
        const auto byte = static_cast<std::uint8_t>(text[offset++]);
        if ((byte & 0xc0) != 0x80) return '?';
        codepoint = (codepoint << 6) | (byte & 0x3f);
    }
    return codepoint;
}
}

BitmapFont parse_bitmap_font(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 8) throw std::runtime_error("truncated bitmap-font header");
    const auto raw_height = bytes[4];
    const bool special = raw_height > 100;
    const auto height = static_cast<std::uint8_t>(special ? raw_height - 100 : raw_height);
    const std::size_t glyph_count = 95 + (special ? 9 : 0);
    const std::size_t metrics_end = 8 + glyph_count * 2;
    if (bytes.size() - metrics_end < 8)
        throw std::runtime_error("truncated bitmap-font metadata");
    std::vector<std::uint16_t> offsets;
    offsets.reserve(glyph_count);
    for (std::size_t i = 0; i < glyph_count; ++i)
        offsets.push_back(read_u16_be(bytes, 8 + i * 2));
    if (!std::is_sorted(offsets.begin(), offsets.end()))
        throw std::runtime_error("bitmap-font glyph offsets are not monotonic");
    const auto png_length = static_cast<std::size_t>(read_u16_be(bytes, metrics_end));
    const auto text_color_offset = read_u16_be(bytes, metrics_end + 4);
    const auto shadow_color_offset = read_u16_be(bytes, metrics_end + 6);
    const auto png_begin = metrics_end + 8;
    if (png_length > bytes.size() - png_begin || png_begin + png_length != bytes.size())
        throw std::runtime_error("bitmap-font PNG length mismatch");
    std::vector<std::uint8_t> png(bytes.begin() + static_cast<std::ptrdiff_t>(png_begin),
                                  bytes.end());
    auto atlas = decode_indexed_png(png);
    if (height == 0 || atlas.height < height || offsets.back() > atlas.width)
        throw std::runtime_error("bitmap-font metrics exceed atlas dimensions");
    if (text_color_offset == 0xffff || text_color_offset > png.size() ||
        png.size() - text_color_offset < 3)
        throw std::runtime_error("bitmap-font text palette offset is invalid");
    const bool has_shadow = shadow_color_offset != 0 && shadow_color_offset != 0xffff;
    if (has_shadow && (shadow_color_offset > png.size() || png.size() - shadow_color_offset < 3))
        throw std::runtime_error("bitmap-font shadow palette offset is invalid");
    const auto text_rgb = read_rgb(png, text_color_offset);
    const auto shadow_rgb = has_shadow ? read_rgb(png, shadow_color_offset) : 0;
    return {height, bytes[5], bytes[6], bytes[7], special, std::move(offsets),
            std::move(atlas), text_rgb, shadow_rgb, has_shadow, {}};
}

BitmapFont load_bitmap_font(const GameArchive& archive, const std::string& path) {
    return parse_bitmap_font(archive.read_entry(path));
}

void add_bitmap_glyphs(BitmapFont& font, const BitmapFont& source,
                       const std::vector<std::pair<std::uint32_t, std::uint32_t>>& glyphs) {
    const auto height = std::max(font.atlas.height, source.atlas.height);
    auto& atlas = font.atlas;
    std::uint32_t added = 0;
    for (const auto& glyph : glyphs) {
        const auto g = std::size_t(glyph.second) - 33;
        if (glyph.second < 33 || g + 1 >= source.glyph_offsets.size()) continue;
        added += source.glyph_offsets[g + 1] - source.glyph_offsets[g];
    }
    const auto old_width = atlas.width, width = old_width + added;
    std::vector<std::uint8_t> rgba(std::size_t(width) * height * 4, 0);
    for (std::uint32_t y = 0; y < atlas.height; ++y)
        std::copy_n(atlas.rgba.begin() + std::ptrdiff_t(y) * old_width * 4, std::size_t(old_width) * 4,
                    rgba.begin() + std::ptrdiff_t(y) * width * 4);
    std::uint32_t pen = old_width;
    for (const auto& glyph : glyphs) {
        const auto g = std::size_t(glyph.second) - 33;
        if (glyph.second < 33 || g + 1 >= source.glyph_offsets.size()) continue;
        const std::uint32_t left = source.glyph_offsets[g], span = source.glyph_offsets[g + 1] - left;
        for (std::uint32_t y = 0; y < source.atlas.height; ++y)
            std::copy_n(source.atlas.rgba.begin() + (std::ptrdiff_t(y) * source.atlas.width + left) * 4, std::size_t(span) * 4,
                        rgba.begin() + (std::ptrdiff_t(y) * width + pen) * 4);
        font.extra_glyphs.push_back({glyph.first, pen, span});
        pen += span;
    }
    atlas.width = width;
    atlas.height = height;
    atlas.rgba = std::move(rgba);
    font.height = std::max(font.height, source.height);
}

void add_marked_glyph(BitmapFont& font, std::uint32_t base, std::uint32_t added, bool diaeresis) {
    std::uint32_t left = 0;
    int width = 0;
    if (!glyph_span(base, font, left, width) || width <= 0) return;
    auto& atlas = font.atlas;
    const auto pixel = [&](std::uint32_t x, std::uint32_t y) { return &atlas.rgba[(std::size_t(y) * atlas.width + x) * 4]; };
    int top = -1, first = width, last = -1;
    for (std::uint32_t y = 0; y < atlas.height && top < 0; ++y)
        for (int x = 0; x < width; ++x) if (pixel(left + x, y)[3]) { top = int(y); break; }
    if (top < 1) return; // no room above the letter
    for (std::uint32_t y = 0; y < atlas.height; ++y)
        for (int x = 0; x < width; ++x) if (pixel(left + x, y)[3]) { first = std::min(first, x); last = std::max(last, x); }
    std::array<std::uint8_t, 4> ink{};
    for (int x = 0; x < width; ++x) if (pixel(left + x, top)[3]) { std::copy_n(pixel(left + x, top), 4, ink.begin()); break; }
    // Copy the base glyph to a new column span at the right edge of the atlas.
    const auto old_width = atlas.width, new_width = old_width + std::uint32_t(width);
    std::vector<std::uint8_t> rgba(std::size_t(new_width) * atlas.height * 4, 0);
    for (std::uint32_t y = 0; y < atlas.height; ++y) {
        std::copy_n(atlas.rgba.begin() + std::ptrdiff_t(y) * old_width * 4, std::size_t(old_width) * 4,
                    rgba.begin() + std::ptrdiff_t(y) * new_width * 4);
        std::copy_n(atlas.rgba.begin() + (std::ptrdiff_t(y) * old_width + left) * 4, std::size_t(width) * 4,
                    rgba.begin() + (std::ptrdiff_t(y) * new_width + old_width) * 4);
    }
    atlas.rgba = std::move(rgba);
    atlas.width = new_width;
    const int row = top >= 2 ? top - 2 : top - 1; // leave a gap row when there is space
    const auto put = [&](int x) {
        if (x >= 0 && x < width) std::copy_n(ink.begin(), 4, &atlas.rgba[(std::size_t(row) * new_width + old_width + std::uint32_t(x)) * 4]);
    };
    if (diaeresis) {
        const bool narrow = last - first <= 3;
        put(narrow ? first : first + 1);
        put(narrow ? last : last - 1);
    } else {
        for (int x = first + 1; x < last; ++x) put(x);
    }
    font.extra_glyphs.erase(std::remove_if(font.extra_glyphs.begin(), font.extra_glyphs.end(),
        [&](const BitmapExtraGlyph& g) { return g.codepoint == added; }), font.extra_glyphs.end());
    font.extra_glyphs.push_back({added, old_width, std::uint32_t(width)});
}

void recolor_bitmap_font(BitmapFont& font, std::uint32_t text_rgb,
                         std::uint32_t shadow_rgb) {
    // The PNG decoder expands palette entries; recolor matching atlas pixels.
    replace_rgb(font.atlas, font.text_palette_rgb, text_rgb);
    if (font.has_shadow_palette)
        replace_rgb(font.atlas, font.shadow_palette_rgb, shadow_rgb);
}

std::vector<BitmapGlyphPlacement> layout_bitmap_text(
    const BitmapFont& font, const std::string& utf8, int x, int y,
    int max_width, int scale) {
    if (scale < 1 || max_width < 1) throw std::invalid_argument("invalid font layout bounds");
    if(font.height==0 || font.glyph_offsets.size()<95) throw std::invalid_argument("font metrics missing");
    std::vector<BitmapGlyphPlacement> result;
    const int right = x + max_width;
    int pen_x = x, pen_y = y;
    const auto line_height = static_cast<int>(font.height) * scale;
    for (std::size_t i = 0; i < utf8.size();) {
        const auto character_begin=i;
        auto codepoint = next_codepoint(utf8, i);
        if(codepoint!=' ' && codepoint!='\n' && codepoint!='\r' &&
           (character_begin==0 || utf8[character_begin-1]==' ' || utf8[character_begin-1]=='\n')) {
            // Keep a complete word on the next line when it fits there. Long
            // tokens still use the per-glyph fallback below.
            std::size_t look=character_begin; int word_width=0;
            while(look<utf8.size()) {
                auto c=next_codepoint(utf8,look);
                if(c==' ' || c=='\n' || c=='\r') break;
                std::uint32_t left=0;int width=0;
                if(!glyph_span(c,font,left,width)) continue;
                word_width+=std::max(1,(width-font.spacing)*scale);
                if(word_width>max_width) break;
            }
            if(word_width<=max_width && pen_x!=x && pen_x+word_width>right) {pen_x=x;pen_y+=line_height;}
        }
        if (codepoint == '\r') continue;
        if (codepoint == '\n') {
            pen_x = x;
            pen_y += line_height;
            continue;
        }
        if (codepoint == ' ') {
            const int advance = std::max(1, (static_cast<int>(font.space_width) - font.spacing) * scale);
            if (pen_x + advance > right && pen_x != x) {
                pen_x = x;
                pen_y += line_height;
            } else {
                pen_x += advance;
            }
            continue;
        }
        std::uint32_t left = 0;
        int glyph_width = 0;
        if (!glyph_span(codepoint, font, left, glyph_width)) continue;
        const int advance = std::max(1, (glyph_width - font.spacing) * scale);
        if (pen_x + advance > right && pen_x != x) {
            pen_x = x;
            pen_y += line_height;
        }
        result.push_back({left, static_cast<std::uint32_t>(glyph_width), pen_x, pen_y,i});
        pen_x += advance;
    }
    return result;
}
