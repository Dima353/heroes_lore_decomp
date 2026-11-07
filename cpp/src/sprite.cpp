#include "sprite.hpp"

#include "map.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <map>
#include <stdexcept>
#include <zlib.h>

namespace {
std::uint16_t be16(const std::vector<std::uint8_t>& data, std::size_t pos) {
    if (pos > data.size() || data.size() - pos < 2) throw std::runtime_error("truncated MPH field");
    return static_cast<std::uint16_t>((std::uint16_t(data[pos]) << 8) | data[pos + 1]);
}

std::uint32_t be32(const std::vector<std::uint8_t>& data, std::size_t pos) {
    if (pos > data.size() || data.size() - pos < 4) throw std::runtime_error("truncated sprite field");
    return (std::uint32_t(data[pos]) << 24) | (std::uint32_t(data[pos + 1]) << 16) |
           (std::uint32_t(data[pos + 2]) << 8) | data[pos + 3];
}

void append(std::vector<std::uint8_t>& out, const std::uint8_t* begin, std::size_t size) {
    out.insert(out.end(), begin, begin + size);
}

struct Chunk {
    std::array<char, 4> type{};
    std::size_t begin{};
    std::size_t end{};
};

std::vector<Chunk> chunks(const std::vector<std::uint8_t>& bytes, std::size_t begin, std::size_t end) {
    if (begin > end || end > bytes.size()) throw std::runtime_error("invalid MPD frame range");
    std::vector<Chunk> result;
    std::size_t pos = begin;
    while (pos < end) {
        if (end - pos < 12) throw std::runtime_error("truncated PNG chunk in MPD");
        const auto length = static_cast<std::size_t>(be32(bytes, pos));
        if (length > end - pos - 12) throw std::runtime_error("PNG chunk exceeds MPD frame boundary");
        Chunk chunk;
        for (std::size_t i = 0; i < 4; ++i) chunk.type[i] = static_cast<char>(bytes[pos + 4 + i]);
        chunk.begin = pos;
        chunk.end = pos + 12 + length;
        uLong checksum = crc32(0L, Z_NULL, 0);
        checksum = crc32(checksum, bytes.data() + pos + 4, 4);
        std::size_t payload_pos = pos + 8;
        std::size_t payload_left = length;
        while (payload_left != 0) {
            const auto block = static_cast<uInt>(std::min<std::size_t>(
                payload_left, std::numeric_limits<uInt>::max()));
            checksum = crc32(checksum, bytes.data() + payload_pos, block);
            payload_pos += block;
            payload_left -= block;
        }
        if (static_cast<std::uint32_t>(checksum) != be32(bytes, pos + 8 + length))
            throw std::runtime_error("PNG chunk CRC mismatch in MPD");
        result.push_back(chunk);
        pos = chunk.end;
    }
    return result;
}

bool is(const Chunk& chunk, const char* name) {
    return std::equal(chunk.type.begin(), chunk.type.end(), name);
}

std::vector<std::uint8_t> reconstruct(const SpriteAtlas& atlas,
                                     const std::vector<std::uint8_t>& mpd,
                                     std::size_t frame_index, std::size_t frame_end) {
    const auto& ref = atlas.frames.at(frame_index);
    if (ref.offset >= frame_end || frame_end > mpd.size())
        throw std::runtime_error("invalid MPH frame offset");
    const auto source_chunks = chunks(mpd, ref.offset, frame_end);
    if (source_chunks.empty()) throw std::runtime_error("empty sprite frame");

    std::vector<std::uint8_t> out{0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    if (!atlas.merged()) {
        if (!is(source_chunks.front(), "IHDR") || !is(source_chunks.back(), "IDAT"))
            throw std::runtime_error("simple sprite frame must start with IHDR and end with IDAT");
        const auto ihdr_size = source_chunks.front().end - source_chunks.front().begin;
        append(out, mpd.data() + source_chunks.front().begin, ihdr_size);
        out.insert(out.end(), atlas.palette_chunks.begin(), atlas.palette_chunks.end());
        const auto tail_begin = source_chunks.front().end;
        append(out, mpd.data() + tail_begin, frame_end - tail_begin);
    } else {
        const Chunk* ihdr = nullptr;
        const Chunk* idat = nullptr;
        std::map<std::array<char, 4>, const Chunk*> ancillary;
        for (const auto& chunk : source_chunks) {
            if (is(chunk, "IHDR")) { if (!ihdr) ihdr = &chunk; }
            else if (is(chunk, "IDAT")) { if (!idat) idat = &chunk; }
            else ancillary.emplace(chunk.type, &chunk);
        }
        if (!ihdr || !idat) throw std::runtime_error("merged sprite frame has no IHDR or IDAT");
        append(out, mpd.data() + ihdr->begin, ihdr->end - ihdr->begin);
        const std::array<const char*, 7> before{{"cHRM", "gAMA", "iCCP", "sBIT", "sRGB", "pHYs", "sPLT"}};
        const std::array<int, 7> before_bits{{0, 1, 2, 3, 4, 8, 9}};
        for (std::size_t i = 0; i < before.size(); ++i) {
            if ((ref.chunk_mask & (1u << before_bits[i])) == 0) continue;
            for (const auto& pair : ancillary) if (is(*pair.second, before[i]))
                append(out, mpd.data() + pair.second->begin, pair.second->end - pair.second->begin);
        }
        out.insert(out.end(), atlas.palette_chunks.begin(), atlas.palette_chunks.end());
        const std::array<const char*, 2> after{{"hIST", "bKGD"}};
        const std::array<int, 2> after_bits{{13, 14}};
        for (std::size_t i = 0; i < after.size(); ++i) {
            if ((ref.chunk_mask & (1u << after_bits[i])) == 0) continue;
            for (const auto& pair : ancillary) if (is(*pair.second, after[i]))
                append(out, mpd.data() + pair.second->begin, pair.second->end - pair.second->begin);
        }
        append(out, mpd.data() + idat->begin, idat->end - idat->begin);
    }
    static constexpr std::array<std::uint8_t, 12> iend{{0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xae, 0x42, 0x60, 0x82}};
    out.insert(out.end(), iend.begin(), iend.end());
    return out;
}
} // namespace

SpriteAtlas load_sprite_atlas(const std::string& jar, const std::string& stem) {
    return load_sprite_atlas(GameArchive(jar), stem);
}

SpriteAtlas load_sprite_atlas(const GameArchive& archive, const std::string& stem) {
    if (stem.empty() || stem.front() == '/' || stem.find("..") != std::string::npos ||
        stem.find('\\') != std::string::npos)
        throw std::invalid_argument("invalid sprite atlas path");
    auto header = archive.read_entry(stem + ".mph");
    if (header.size() < 8) throw std::runtime_error("truncated MPH header");
    SpriteAtlas atlas;
    atlas.flags = be32(header, 0);
    const auto count = static_cast<std::size_t>(be32(header, 4));
    if (count > (std::numeric_limits<std::size_t>::max() - 8) / 8 || 8 + count * 8 > header.size())
        throw std::runtime_error("truncated MPH frame table");
    atlas.frames.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto pos = 8 + i * 8;
        atlas.frames.push_back({be16(header, pos), be32(header, pos + 2), be16(header, pos + 6)});
    }
    for (std::size_t i = 1; i < atlas.frames.size(); ++i)
        if (atlas.frames[i].offset < atlas.frames[i - 1].offset)
            throw std::runtime_error("MPH frame offsets are not sorted");
    atlas.palette_chunks.assign(header.begin() + static_cast<std::ptrdiff_t>(8 + count * 8), header.end());
    (void)chunks(atlas.palette_chunks, 0, atlas.palette_chunks.size());
    for (const auto& chunk : chunks(atlas.palette_chunks, 0, atlas.palette_chunks.size()))
        if (!is(chunk, "PLTE") && !is(chunk, "tRNS"))
            throw std::runtime_error("unexpected chunk after MPH frame table");
    return atlas;
}

std::vector<std::uint8_t> load_sprite_frame_png(
    const std::string& jar, const std::string& stem, std::size_t frame_index) {
    return load_sprite_frame_png(GameArchive(jar), stem, frame_index);
}

std::vector<std::uint8_t> load_sprite_frame_png(
    const GameArchive& archive, const std::string& stem, std::size_t frame_index) {
    auto frames = load_sprite_frames_png(archive, stem, {frame_index});
    return std::move(frames.front().second);
}

std::vector<std::pair<std::size_t, std::vector<std::uint8_t>>> load_sprite_frames_png(
    const std::string& jar, const std::string& stem,
    const std::vector<std::size_t>& frame_indices) {
    return load_sprite_frames_png(GameArchive(jar), stem, frame_indices);
}

std::vector<std::pair<std::size_t, std::vector<std::uint8_t>>> load_sprite_frames_png(
    const GameArchive& archive, const std::string& stem,
    const std::vector<std::size_t>& frame_indices) {
    const auto atlas = load_sprite_atlas(archive, stem);
    std::map<std::uint16_t, std::vector<std::uint8_t>> mpd_cache;
    std::vector<std::pair<std::size_t, std::vector<std::uint8_t>>> result;
    result.reserve(frame_indices.size());
    for (const auto frame_index : frame_indices) {
        if (frame_index >= atlas.frames.size()) throw std::out_of_range("sprite frame index outside atlas");
        const auto& ref = atlas.frames[frame_index];
        auto mpd_it = mpd_cache.find(ref.file_id);
        if (mpd_it == mpd_cache.end()) {
            auto data = archive.read_entry(stem + "_" + std::to_string(ref.file_id) + ".mpd");
            mpd_it = mpd_cache.emplace(ref.file_id, std::move(data)).first;
        }
        std::size_t end = mpd_it->second.size();
        if (frame_index + 1 < atlas.frames.size() &&
            atlas.frames[frame_index + 1].file_id == ref.file_id)
            end = atlas.frames[frame_index + 1].offset;
        result.emplace_back(frame_index, reconstruct(atlas, mpd_it->second, frame_index, end));
    }
    return result;
}

DecodedImage decode_indexed_png(const std::vector<std::uint8_t>& png) {
    static constexpr std::array<std::uint8_t, 8> signature{{0x89, 'P', 'N', 'G', 13, 10, 26, 10}};
    if (png.size() < signature.size() || !std::equal(signature.begin(), signature.end(), png.begin()))
        throw std::runtime_error("invalid PNG signature");
    std::uint32_t width = 0, height = 0;
    unsigned bit_depth = 0, color_type = 0, interlace = 0;
    std::vector<std::uint8_t> palette, transparency, compressed;
    std::size_t pos = signature.size();
    bool have_ihdr = false, have_iend = false;
    while (pos < png.size()) {
        if (png.size() - pos < 12) throw std::runtime_error("truncated PNG chunk");
        const auto length = static_cast<std::size_t>(be32(png, pos));
        if (length > png.size() - pos - 12) throw std::runtime_error("PNG chunk exceeds input size");
        const auto* type = png.data() + pos + 4;
        const auto* payload = png.data() + pos + 8;
        if (std::equal(type, type + 4, "IHDR")) {
            if (have_ihdr || length != 13) throw std::runtime_error("invalid PNG IHDR");
            width = be32(png, pos + 8);
            height = be32(png, pos + 12);
            bit_depth = png[pos + 16];
            color_type = png[pos + 17];
            interlace = png[pos + 20];
            have_ihdr = true;
        } else if (std::equal(type, type + 4, "PLTE")) {
            palette.assign(payload, payload + length);
        } else if (std::equal(type, type + 4, "tRNS")) {
            transparency.assign(payload, payload + length);
        } else if (std::equal(type, type + 4, "IDAT")) {
            if (length > compressed.max_size() - compressed.size())
                throw std::runtime_error("PNG compressed data is too large");
            compressed.insert(compressed.end(), payload, payload + length);
        } else if (std::equal(type, type + 4, "IEND")) {
            if (length != 0) throw std::runtime_error("invalid PNG IEND");
            have_iend = true;
            break;
        }
        pos += 12 + length;
    }
    // Indexed images are the norm; some localized fr-FR images (Russian release) are 8-bit RGB/RGBA.
    const bool truecolor = (color_type == 2 || color_type == 6) && bit_depth == 8;
    if (!have_ihdr || !have_iend || !width || !height || (color_type != 3 && !truecolor) || interlace != 0)
        throw std::runtime_error("expected a non-interlaced indexed PNG");
    const unsigned channels = color_type == 2 ? 3u : color_type == 6 ? 4u : 1u;
    if (width > static_cast<std::uint32_t>(std::numeric_limits<int>::max() / 4) ||
        height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("indexed PNG dimensions exceed SDL limits");
    if (bit_depth != 1 && bit_depth != 2 && bit_depth != 4 && bit_depth != 8)
        throw std::runtime_error("unsupported indexed PNG bit depth");
    if (!truecolor && (palette.empty() || palette.size() % 3 != 0 || palette.size() > 768))
        throw std::runtime_error("invalid indexed PNG palette");
    if (static_cast<std::size_t>(width) >
        (std::numeric_limits<std::size_t>::max() - 7) / (bit_depth * channels))
        throw std::runtime_error("indexed PNG row dimensions overflow");
    const std::size_t bits_per_row = static_cast<std::size_t>(width) * bit_depth * channels;
    const std::size_t pixel_bytes = std::max<std::size_t>(1, bit_depth * channels / 8);
    const std::size_t row_bytes = (bits_per_row + 7) / 8;
    if (height > std::numeric_limits<std::size_t>::max() / (row_bytes + 1))
        throw std::runtime_error("indexed PNG dimensions overflow");
    const std::size_t filtered_size = static_cast<std::size_t>(height) * (row_bytes + 1);
    if (compressed.empty() || filtered_size > std::numeric_limits<uLongf>::max() ||
        compressed.size() > std::numeric_limits<uLong>::max())
        throw std::runtime_error("indexed PNG exceeds zlib limits");
    std::vector<std::uint8_t> filtered(filtered_size);
    uLongf output_size = static_cast<uLongf>(filtered.size());
    const auto zstatus = uncompress(filtered.data(), &output_size, compressed.data(),
                                    static_cast<uLong>(compressed.size()));
    if (zstatus != Z_OK || output_size != filtered.size())
        throw std::runtime_error("cannot inflate indexed PNG image data");

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(height) * row_bytes);
    for (std::size_t y = 0; y < height; ++y) {
        const auto source = y * (row_bytes + 1);
        const auto filter = filtered[source];
        if (filter > 4) throw std::runtime_error("unsupported PNG row filter");
        for (std::size_t x = 0; x < row_bytes; ++x) {
            const int raw = filtered[source + 1 + x];
            const int left = x >= pixel_bytes ? pixels[y * row_bytes + x - pixel_bytes] : 0;
            const int above = y ? pixels[(y - 1) * row_bytes + x] : 0;
            const int upper_left = (y && x >= pixel_bytes) ? pixels[(y - 1) * row_bytes + x - pixel_bytes] : 0;
            int predictor = 0;
            if (filter == 1) predictor = left;
            else if (filter == 2) predictor = above;
            else if (filter == 3) predictor = (left + above) / 2;
            else if (filter == 4) {
                const int p = left + above - upper_left;
                const int pa = std::abs(p - left), pb = std::abs(p - above), pc = std::abs(p - upper_left);
                predictor = pa <= pb && pa <= pc ? left : (pb <= pc ? above : upper_left);
            }
            pixels[y * row_bytes + x] = static_cast<std::uint8_t>(raw + predictor);
        }
    }

    if (static_cast<std::size_t>(width) > std::numeric_limits<std::size_t>::max() / height / 4)
        throw std::runtime_error("indexed PNG output dimensions overflow");
    DecodedImage image{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
    if (truecolor) {
        // tRNS for RGB holds one 16-bit colour key per channel.
        const bool keyed = color_type == 2 && transparency.size() >= 6;
        for (std::size_t i = 0; i < static_cast<std::size_t>(width) * height; ++i) {
            const auto* in = pixels.data() + i * channels;
            auto* out = image.rgba.data() + i * 4;
            out[0] = in[0]; out[1] = in[1]; out[2] = in[2];
            out[3] = channels == 4 ? in[3] : 255;
            if (keyed && in[0] == transparency[1] && in[1] == transparency[3] && in[2] == transparency[5]) out[3] = 0;
        }
        return image;
    }
    const unsigned mask = (1u << bit_depth) - 1u;
    for (std::size_t y = 0; y < height; ++y) {
        const auto* row = pixels.data() + y * row_bytes;
        for (std::size_t x = 0; x < width; ++x) {
            const std::size_t bit = x * bit_depth;
            const unsigned shift = 8u - bit_depth - static_cast<unsigned>(bit % 8);
            const auto color = static_cast<std::size_t>((row[bit / 8] >> shift) & mask);
            if (color * 3 + 2 >= palette.size()) throw std::runtime_error("PNG palette index outside PLTE");
            const auto out = (y * width + x) * 4;
            image.rgba[out] = palette[color * 3];
            image.rgba[out + 1] = palette[color * 3 + 1];
            image.rgba[out + 2] = palette[color * 3 + 2];
            image.rgba[out + 3] = color < transparency.size() ? transparency[color] : 255;
        }
    }
    return image;
}
