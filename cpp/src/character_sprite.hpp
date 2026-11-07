#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class GameArchive;

struct CharacterSpriteLayer {
    std::int8_t x{};
    std::int8_t y{};
    bool mirrored{};
    std::uint8_t atlas_frame{};
};

using CharacterSpriteFrame = std::vector<CharacterSpriteLayer>;

struct CharacterSpriteRecord {
    // Key bytes used by the original renderer: layer slot, animation state,
    // and direction (stored as found in the resource table).
    std::uint8_t layer_slot{};
    std::uint8_t animation_state{};
    std::uint8_t direction{};
    // For a/b/hA/hB/w/s each animation frame contains one layer. Category e
    // can contain multiple layers per frame.
    std::vector<CharacterSpriteFrame> frames;
};

using CharacterSpriteTable = std::vector<CharacterSpriteRecord>;

CharacterSpriteTable parse_character_sprite_table(
    const std::vector<std::uint8_t>& bytes, bool variable_layers_per_frame);

CharacterSpriteTable load_character_sprite_table(
    const GameArchive& archive, int character_class, const std::string& category);

const CharacterSpriteRecord* find_character_sprite_record(
    const CharacterSpriteTable& table, std::uint8_t layer_slot,
    std::uint8_t animation_state, std::uint8_t direction);
