#pragma once

#include "map.hpp"

#include <cstdint>
#include <vector>

struct HeroState {
    int x_pixels{};
    int y_pixels{};
    int x_tile{};
    int y_tile{};
    bool partial_x{};
    bool partial_y{};
    int motion_mode{1};
    int direction{2};
    int target_direction{2};
    int movement_marker{-1};
    int resume_direction{};
};

class HeroInput {
public:
    HeroInput(const GameMap& map, const EventGrid& event,
              const std::vector<MapEntity>& npcs,
              const std::vector<MapEntity>& enemies);
    HeroState state() const { return hero_; }
    void sync_world(const EventGrid& event,
                    const std::vector<MapEntity>& npcs,
                    const std::vector<MapEntity>& enemies);
    void place_on_tile(int x, int y);
    // ao.a(byte,byte): shift the physical position; tile/half-step follow (ck.b), motion is kept.
    void displace(int dx_pixels, int dy_pixels);
    bool press(int direction);
    bool release();
    bool apply_script_command(std::uint8_t opcode, std::uint8_t direction);
    bool tick();
    bool blocked() const { return blocked_; } // o.a()Z: the last step was refused at a tile edge
private:
    bool contains(int x, int y) const;
    bool passable(int x, int y) const;
    bool can_place(int x, int y) const;
    bool tick_base_motion();
    bool tick_hero_motion();
    bool apply_pending();
    HeroState hero_;
    int width_{};
    int height_{};
    std::vector<std::uint8_t> overlay_;
    std::vector<std::uint8_t> occupied_;
    int pending_mode_{};
    int pending_direction_{};
    bool blocked_{};
};
