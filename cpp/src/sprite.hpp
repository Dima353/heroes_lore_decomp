#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct SpriteFrameRef {
    std::uint16_t file_id{};
    std::uint32_t offset{};
    std::uint16_t chunk_mask{};
};

struct SpriteAtlas {
    std::uint32_t flags{};
    std::vector<SpriteFrameRef> frames;
    std::vector<std::uint8_t> palette_chunks;
    bool merged() const { return (flags & 0x08000000u) != 0; }
    bool mirror_on_load() const { return (flags & 0x04000000u) != 0; }
};

struct DecodedImage {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba;
};

class GameArchive;
SpriteAtlas load_sprite_atlas(const std::string& jar, const std::string& stem);
SpriteAtlas load_sprite_atlas(const GameArchive& archive, const std::string& stem);
std::vector<std::uint8_t> load_sprite_frame_png(
    const std::string& jar, const std::string& stem, std::size_t frame_index);
std::vector<std::uint8_t> load_sprite_frame_png(
    const GameArchive& archive, const std::string& stem, std::size_t frame_index);
std::vector<std::pair<std::size_t, std::vector<std::uint8_t>>> load_sprite_frames_png(
    const std::string& jar, const std::string& stem,
    const std::vector<std::size_t>& frame_indices);
std::vector<std::pair<std::size_t, std::vector<std::uint8_t>>> load_sprite_frames_png(
    const GameArchive& archive, const std::string& stem,
    const std::vector<std::size_t>& frame_indices);
DecodedImage decode_indexed_png(const std::vector<std::uint8_t>& png);
