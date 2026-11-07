#pragma once

#include "event_runtime.hpp"
#include "hero_movement.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

struct NpcMotionState {
    std::int16_t x_pixels{};
    std::int16_t y_pixels{};
    std::int8_t x_tile{};
    std::int8_t y_tile{};
    bool partial_x{};
    bool partial_y{};
    std::uint8_t motion_mode{1};
    std::uint8_t direction{2};
    std::uint8_t target_direction{2};
    std::uint8_t animation_frame{};
    std::uint32_t animation_tick{}; // logic tick of the last frame advance (one step per game frame)
    std::int8_t movement_marker{-1};
};

std::vector<NpcMotionState> make_npc_motion_states(const EventGrid& event);
void apply_npc_movement_command(const EventCommand& command,
                               std::vector<NpcMotionState>& npcs);
std::vector<std::size_t> tick_npc_movement(
    std::vector<NpcMotionState>& npcs, const GameMap& map, const EventGrid& event,
    const std::vector<MapEntity>& enemies, const HeroState& hero);
