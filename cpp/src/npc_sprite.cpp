#include "npc_sprite.hpp"

#include "map.hpp"
#include "sprite.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>

static NpcSpriteAnimations load_animation_table(
    const GameArchive& archive, const char* path, const char* atlas_stem);

std::vector<NpcSpriteLayer> load_npc_idle_frame(
    const std::string& jar, int npc_code, int direction) {
    return load_npc_idle_frame(GameArchive(jar), npc_code, direction);
}

std::vector<NpcSpriteLayer> load_npc_idle_frame(
    const GameArchive& archive, int npc_code, int direction) {
    if (npc_code < 0 || npc_code >= 18) throw std::out_of_range("animated NPC code must be in 0..17");
    if (direction < 1 || direction > 4) throw std::out_of_range("sprite direction must be in 1..4");
    char table[32], atlas[16];
    std::snprintf(table, sizeof(table), "npc/spr/%02d", npc_code);
    std::snprintf(atlas, sizeof(atlas), "npc/%02d", npc_code);
    const auto animations = load_animation_table(archive, table, atlas);
    const auto& frames = animations.idle[static_cast<std::size_t>(direction - 1)];
    return frames.empty() ? std::vector<NpcSpriteLayer>{} : frames.front();
}

NpcSpriteAnimations load_npc_animations(const GameArchive& archive, int npc_code) {
    if (npc_code < 0 || npc_code >= 18)
        throw std::out_of_range("animated NPC code must be in 0..17");
    char table[32], atlas[16];
    std::snprintf(table, sizeof(table), "npc/spr/%02d", npc_code);
    std::snprintf(atlas, sizeof(atlas), "npc/%02d", npc_code);
    auto animations = load_animation_table(archive, table, atlas);
    for (std::size_t direction = 0; direction < 4; ++direction) {
        if (animations.idle[direction].empty() || animations.walk[direction].empty())
            throw std::runtime_error("NPC sprite file is missing idle/walk direction data");
    }
    return animations;
}

NpcSpriteAnimations load_enemy_animations(const GameArchive& archive, int enemy_id) {
    if (enemy_id < 0 || enemy_id > 31)
        throw std::out_of_range("enemy id must be in 0..31");
    char table[32], atlas[16];
    std::snprintf(table, sizeof(table), "enm/spr/%02d", enemy_id);
    std::snprintf(atlas, sizeof(atlas), "enm/%02d", enemy_id);
    auto animations = load_animation_table(archive, table, atlas);
    for (std::size_t direction = 0; direction < 4; ++direction) {
        if (animations.idle[direction].empty() || animations.walk[direction].empty())
            throw std::runtime_error("enemy sprite file is missing idle/walk direction data");
    }
    return animations;
}

std::vector<NpcSpriteLayer> load_enemy_idle_frame(
    const GameArchive& archive, int enemy_id, int direction) {
    if (enemy_id < 0 || enemy_id > 31) throw std::out_of_range("enemy id must be in 0..31");
    if (direction < 1 || direction > 4) throw std::out_of_range("sprite direction must be in 1..4");
    char table[32], atlas[16];
    std::snprintf(table, sizeof(table), "enm/spr/%02d", enemy_id);
    std::snprintf(atlas, sizeof(atlas), "enm/%02d", enemy_id);
    const auto animations = load_animation_table(archive, table, atlas);
    const auto& frames = animations.idle[static_cast<std::size_t>(direction - 1)];
    if (frames.empty()) throw std::runtime_error("enemy sprite file has no idle animation for direction");
    return frames.empty() ? std::vector<NpcSpriteLayer>{} : frames.front();
}

static std::vector<std::array<NpcAnimationFrames, 4>> load_animation_types(
    const GameArchive& archive, const char* path, const char* atlas_stem, std::uint8_t max_type) {
    const auto bytes = archive.read_entry(path);
    const bool mirror_on_load = load_sprite_atlas(archive, atlas_stem).mirror_on_load();
    std::size_t pos = 0;
    std::vector<std::array<NpcAnimationFrames, 4>> animations(std::size_t(max_type) + 1);
    bool found[6][4]{};
    while (pos < bytes.size()) {
        if (bytes.size() - pos < 3) throw std::runtime_error("truncated NPC sprite record header");
        const auto type = bytes[pos++];
        const auto group = bytes[pos++];
        const auto length = static_cast<std::size_t>(bytes[pos++]);
        if (length > bytes.size() - pos) throw std::runtime_error("NPC sprite record exceeds file size");
        const auto end = pos + length;
        if (type > max_type || group > 3) throw std::runtime_error("invalid sprite animation key");
        if (length == 0) throw std::runtime_error("empty NPC sprite animation record");
        const auto frame_count = static_cast<std::size_t>(bytes[pos++]);
        NpcAnimationFrames decoded_frames;
        decoded_frames.reserve(frame_count);
        for (std::size_t frame = 0; frame < frame_count; ++frame) {
            if (pos >= end) throw std::runtime_error("truncated NPC animation frame");
            const auto layer_count = static_cast<std::size_t>(bytes[pos++]);
            if (layer_count > (end - pos) / 4) throw std::runtime_error("truncated NPC sprite layers");
            std::vector<NpcSpriteLayer> layers;
            layers.reserve(layer_count);
            for (std::size_t layer = 0; layer < layer_count; ++layer) {
                const auto raw_x = bytes[pos++];
                const auto raw_y = bytes[pos++];
                const auto raw_mirror = bytes[pos++];
                const auto atlas_frame = bytes[pos++];
                layers.push_back({
                    static_cast<std::int8_t>(raw_x >= 128 ? int(raw_x) - 256 : raw_x),
                    static_cast<std::int8_t>(raw_y >= 128 ? int(raw_y) - 256 : raw_y),
                    raw_mirror != 0 && mirror_on_load, atlas_frame});
            }
            decoded_frames.push_back(std::move(layers));
        }
        if (pos != end) throw std::runtime_error("NPC animation record has trailing bytes");
        if (found[type][group]) throw std::runtime_error("duplicate NPC animation key");
        animations[type][group] = std::move(decoded_frames);
        found[type][group] = true;
    }
    return animations;
}

static NpcSpriteAnimations load_animation_table(
    const GameArchive& archive, const char* path, const char* atlas_stem) {
    auto types = load_animation_types(archive, path, atlas_stem, 2);
    NpcSpriteAnimations animations;
    animations.idle = std::move(types[0]);
    animations.walk = std::move(types[1]);
    animations.attack = std::move(types[2]);
    return animations;
}

BossSpriteAnimations load_boss_animations(const GameArchive& archive, int boss_file, int stats_id) {
    // ce.e(byte): /boss/spr/{file}_{id} types 0 idle, 1 walk, 2 death, 3 attack; cc (file 1)
    // keeps types 3..5 as its three attack sets. All share the /boss/{file} atlas.
    char table[32], atlas[16];
    std::snprintf(table, sizeof(table), "boss/spr/%d_%d", boss_file, stats_id);
    std::snprintf(atlas, sizeof(atlas), "boss/%d", boss_file);
    auto types = load_animation_types(archive, table, atlas, 5);
    BossSpriteAnimations animations;
    for (std::size_t i = 0; i < animations.types.size(); ++i) animations.types[i] = std::move(types[i]);
    return animations;
}
