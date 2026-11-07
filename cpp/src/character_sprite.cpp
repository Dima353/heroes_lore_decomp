#include "character_sprite.hpp"

#include "map.hpp"

#include <array>
#include <stdexcept>
#include <utility>

namespace {
std::int8_t signed_byte(std::uint8_t value) {
    return static_cast<std::int8_t>(value >= 128 ? static_cast<int>(value) - 256 : value);
}

std::uint8_t take_byte(const std::vector<std::uint8_t>& bytes, std::size_t& pos,
                       const char* what) {
    if (pos >= bytes.size()) throw std::runtime_error(std::string("truncated character sprite ") + what);
    return bytes[pos++];
}

CharacterSpriteLayer take_layer(const std::vector<std::uint8_t>& bytes, std::size_t& pos) {
    if (bytes.size() - pos < 4) throw std::runtime_error("truncated character sprite layer");
    const auto x = signed_byte(bytes[pos++]);
    const auto y = signed_byte(bytes[pos++]);
    const bool mirrored = bytes[pos++] != 0;
    const auto atlas_frame = bytes[pos++];
    return {x, y, mirrored, atlas_frame};
}
}

CharacterSpriteTable parse_character_sprite_table(
    const std::vector<std::uint8_t>& bytes, bool variable_layers_per_frame) {
    CharacterSpriteTable table;
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        if (bytes.size() - pos < 4)
            throw std::runtime_error("truncated character sprite record key");

        CharacterSpriteRecord record;
        record.layer_slot = bytes[pos++];
        record.animation_state = bytes[pos++];
        record.direction = bytes[pos++];
        const auto count = static_cast<std::size_t>(bytes[pos++]);

        if (variable_layers_per_frame) {
            record.frames.reserve(count);
            for (std::size_t frame_index = 0; frame_index < count; ++frame_index) {
                const auto layer_count = static_cast<std::size_t>(
                    take_byte(bytes, pos, "frame layer count"));
                if (layer_count > (bytes.size() - pos) / 4)
                    throw std::runtime_error("character sprite frame layers exceed file boundary");
                CharacterSpriteFrame frame;
                frame.reserve(layer_count);
                for (std::size_t layer = 0; layer < layer_count; ++layer)
                    frame.push_back(take_layer(bytes, pos));
                record.frames.push_back(std::move(frame));
            }
        } else {
            if (count > (bytes.size() - pos) / 4)
                throw std::runtime_error("character sprite animation frames exceed file boundary");
            record.frames.reserve(count);
            for (std::size_t frame_index = 0; frame_index < count; ++frame_index) {
                CharacterSpriteFrame frame;
                frame.push_back(take_layer(bytes, pos));
                record.frames.push_back(std::move(frame));
            }
        }
        table.push_back(std::move(record));
    }
    return table;
}

CharacterSpriteTable load_character_sprite_table(
    const GameArchive& archive, int character_class, const std::string& category) {
    if (character_class < 6 || character_class > 8)
        throw std::out_of_range("character class must be in 6..8");
    static const std::array<const char*, 7> categories{{"a", "b", "e", "hA", "hB", "w", "s"}};
    bool valid_category = false;
    for (const auto* valid : categories)
        if (category == valid) valid_category = true;
    if (!valid_category) throw std::invalid_argument("unknown character sprite category");

    const auto path = "c" + std::to_string(character_class - 5) + "/s/" + category;
    const bool variable_layers = category == "e";
    return parse_character_sprite_table(archive.read_entry(path), variable_layers);
}

const CharacterSpriteRecord* find_character_sprite_record(
    const CharacterSpriteTable& table, std::uint8_t layer_slot,
    std::uint8_t animation_state, std::uint8_t direction) {
    for (const auto& record : table) {
        if (record.layer_slot == layer_slot &&
            record.animation_state == animation_state &&
            record.direction == direction)
            return &record;
    }
    return nullptr;
}
