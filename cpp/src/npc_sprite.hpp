#pragma once

#include <cstdint>
#include <array>
#include <string>
#include <vector>

class GameArchive;

struct NpcSpriteLayer {
    std::int8_t x{};
    std::int8_t y{};
    bool mirrored{};
    std::uint8_t frame{};
};

using NpcAnimationFrames = std::vector<std::vector<NpcSpriteLayer>>;

struct NpcSpriteAnimations {
    std::array<NpcAnimationFrames, 4> idle;
    std::array<NpcAnimationFrames, 4> walk;
    std::array<NpcAnimationFrames, 4> attack; // type 2: enemy attack (al draw state 3)
};

struct BossSpriteAnimations {
    std::array<std::array<NpcAnimationFrames, 4>, 6> types; // idle, walk, death, attack, cc sets 2/3
};

NpcSpriteAnimations load_npc_animations(const GameArchive& archive, int npc_code);
BossSpriteAnimations load_boss_animations(const GameArchive& archive, int boss_file, int stats_id);
NpcSpriteAnimations load_enemy_animations(const GameArchive& archive, int enemy_id);

std::vector<NpcSpriteLayer> load_npc_idle_frame(
    const std::string& jar, int npc_code, int direction = 2);
std::vector<NpcSpriteLayer> load_npc_idle_frame(
    const GameArchive& archive, int npc_code, int direction = 2);
std::vector<NpcSpriteLayer> load_enemy_idle_frame(
    const GameArchive& archive, int enemy_id, int direction = 2);
