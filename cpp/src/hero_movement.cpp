#include "hero_movement.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <limits>
#include <stdexcept>

namespace {
constexpr std::array<int, 9> DX{{0, 0, 0, -1, 1, -1, 1, -1, 1}};
constexpr std::array<int, 9> DY{{0, -1, 1, 0, 0, -1, -1, 1, 1}};
struct Detour { int side_a, diagonal_a, side_b, diagonal_b; };
constexpr std::array<Detour, 5> DETOURS{{
    {0, 0, 0, 0}, {3, 5, 4, 6}, {4, 8, 3, 7}, {2, 7, 1, 5}, {1, 6, 2, 8}
}};
int signed_byte(std::uint8_t value) { return value >= 128 ? int(value) - 256 : value; }
int wrapped_byte(int value) { return signed_byte(static_cast<std::uint8_t>(value)); }
}

HeroInput::HeroInput(const GameMap& map, const EventGrid& event,
                     const std::vector<MapEntity>& npcs,
                     const std::vector<MapEntity>& enemies)
    : width_(map.width), height_(map.height),
      occupied_(static_cast<std::size_t>(map.width) * map.height, 0) {
    if (width_ <= 0 || height_ <= 0)
        throw std::invalid_argument("hero movement requires a valid event overlay");
    sync_world(event, npcs, enemies);

    const int center_x = width_ / 2, center_y = height_ / 2;
    int best_distance = std::numeric_limits<int>::max();
    bool found = false;
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            if (!passable(x, y)) continue;
            const int distance = std::abs(x - center_x) + std::abs(y - center_y);
            if (distance < best_distance) {
                best_distance = distance;
                hero_.x_tile = x;
                hero_.y_tile = y;
                found = true;
            }
        }
    }
    if (!found) throw std::runtime_error("map has no free tile for the debug hero");
    hero_.x_pixels = hero_.x_tile * 16;
    hero_.y_pixels = hero_.y_tile * 16;
}

void HeroInput::sync_world(const EventGrid& event,
                           const std::vector<MapEntity>& npcs,
                           const std::vector<MapEntity>& enemies) {
    const auto cell_count = static_cast<std::size_t>(width_) * height_;
    if (event.overlay.size() != cell_count)
        throw std::invalid_argument("hero movement requires a valid event overlay");
    overlay_ = event.overlay;
    occupied_.assign(cell_count, 0);
    const auto mark = [&](const MapEntity& entity) {
        const auto mark_cell = [&](int x, int y) {
            if (x >= 0 && y >= 0 && x < width_ && y < height_)
                occupied_[static_cast<std::size_t>(y) * width_ + x] = 1;
        };
        mark_cell(entity.x, entity.y);
        if (entity.partial_y) mark_cell(entity.x, int(entity.y) + 1);
        else if (entity.partial_x) mark_cell(int(entity.x) + 1, entity.y);
    };
    for (std::size_t i = 0; i < npcs.size(); ++i)
        if (i >= event.npc_visible.size() || event.npc_visible[i]) mark(npcs[i]);
    for (const auto& enemy : enemies) mark(enemy);
}

void HeroInput::place_on_tile(int x, int y) {
    if (!contains(x, y)) throw std::out_of_range("hero start tile outside map");
    hero_.x_tile = x;
    hero_.y_tile = y;
    hero_.x_pixels = x * 16;
    hero_.y_pixels = y * 16;
    hero_.partial_x = hero_.partial_y = false;
    hero_.motion_mode = 1;
    hero_.movement_marker = -1;
    hero_.resume_direction = 0;
    pending_mode_ = pending_direction_ = 0;
}

void HeroInput::displace(int dx_pixels, int dy_pixels) {
    const int x = hero_.x_pixels + dx_pixels, y = hero_.y_pixels + dy_pixels;
    if (x < 0 || y < 0 || x >= width_ * 16 || y >= height_ * 16) return;
    hero_.x_pixels = x;
    hero_.y_pixels = y;
    hero_.x_tile = x / 16;
    hero_.y_tile = y / 16;
    hero_.partial_x = x % 16 != 0;
    hero_.partial_y = y % 16 != 0;
}

bool HeroInput::contains(int x, int y) const {
    return x >= 0 && y >= 0 && x < width_ && y < height_;
}

bool HeroInput::passable(int x, int y) const {
    if (!contains(x, y)) return false;
    const auto index = static_cast<std::size_t>(y) * width_ + x;
    return signed_byte(overlay_[index]) >= 0 && occupied_[index] == 0;
}

bool HeroInput::can_place(int x, int y) const { return passable(x, y); }

bool HeroInput::press(int direction) {
    if (direction < 1 || direction > 4) return false;
    if (hero_.motion_mode == 1) {
        pending_mode_ = pending_direction_ = 0;
        hero_.motion_mode = 2;
        hero_.movement_marker = -1;
        hero_.direction = hero_.target_direction = direction;
        return true;
    }
    if (hero_.motion_mode == 2) {
        pending_mode_ = 2;
        pending_direction_ = direction;
        return true;
    }
    return false;
}

bool HeroInput::release() {
    if (hero_.motion_mode != 2) return false;
    pending_mode_ = 1;
    pending_direction_ = hero_.direction;
    return true;
}

bool HeroInput::apply_script_command(std::uint8_t opcode, std::uint8_t raw_direction) {
    if (opcode == 5) {
        pending_mode_ = pending_direction_ = 0;
        hero_.motion_mode = 1;
        hero_.movement_marker = -1;
        return true;
    }
    if (opcode != 3 && opcode != 6) return false;
    const int direction = raw_direction >= 128 ? int(raw_direction) - 256 : raw_direction;
    if (direction < 1 || direction > 4) return false;
    if (hero_.partial_x || hero_.partial_y) {
        // Turning mid-step would leave the other axis' half-step flag set, which skips
        // every collision check; finish the step first, like apply_pending does.
        pending_mode_ = opcode == 3 ? 2 : hero_.motion_mode == 2 ? 1 : hero_.motion_mode;
        pending_direction_ = direction;
        return true;
    }
    pending_mode_ = pending_direction_ = 0;
    hero_.direction = hero_.target_direction = direction;
    hero_.movement_marker = -1;
    if (opcode == 3) hero_.motion_mode = 2;
    return true;
}

bool HeroInput::apply_pending() {
    if (!pending_mode_ || hero_.partial_x || hero_.partial_y) return false;
    hero_.motion_mode = pending_mode_;
    hero_.movement_marker = -1;
    hero_.direction = hero_.target_direction = pending_direction_;
    pending_mode_ = pending_direction_ = 0;
    return true;
}

bool HeroInput::tick() {
    blocked_ = false;
    apply_pending();
    return tick_hero_motion();
}

bool HeroInput::tick_base_motion() {
    if (hero_.motion_mode != 2 && hero_.motion_mode != 4) return false;
    const int direction = hero_.direction;
    if (direction < 1 || direction > 4) throw std::runtime_error("invalid debug hero direction");
    const int dx = DX[direction], dy = DY[direction];
    if (!hero_.partial_x && !hero_.partial_y &&
        !can_place(hero_.x_tile + dx, hero_.y_tile + dy)) {
        hero_.motion_mode = 1;
        hero_.movement_marker = -1;
        blocked_ = true;
        return false;
    }
    blocked_ = false;
    const int next_x = hero_.x_pixels + 8 * dx;
    const int next_y = hero_.y_pixels + 8 * dy;
    if (next_x < 0 || next_y < 0 || next_x >= width_ * 16 || next_y >= height_ * 16) {
        hero_.motion_mode = 1;
        hero_.movement_marker = -1;
        return false;
    }
    hero_.x_pixels = next_x;
    hero_.y_pixels = next_y;
    if (dx != 0) {
        const bool old_partial = hero_.partial_x;
        hero_.partial_x = !old_partial;
        if ((dx > 0 && old_partial) || (dx < 0 && !old_partial)) hero_.x_tile += dx;
    } else {
        const bool old_partial = hero_.partial_y;
        hero_.partial_y = !old_partial;
        if ((dy > 0 && old_partial) || (dy < 0 && !old_partial)) hero_.y_tile += dy;
    }
    return true;
}

bool HeroInput::tick_hero_motion() {
    const int mode = hero_.motion_mode;
    if (mode != 1 && mode != 2 && mode != 4)
        throw std::runtime_error("unsupported debug hero motion mode");
    int direction = hero_.direction;
    int marker = wrapped_byte(hero_.movement_marker + 1);
    if (mode == 1) {
        hero_.resume_direction = 0;
        hero_.movement_marker = marker == 1 ? 0 : marker;
        return false;
    }
    if (mode == 2) {
        if (hero_.resume_direction && !hero_.partial_x && !hero_.partial_y) {
            direction = hero_.resume_direction;
            hero_.direction = hero_.target_direction = direction;
            hero_.resume_direction = 0;
        }
        if (marker == 4) marker = 0;
    }
    hero_.movement_marker = marker;
    if (tick_base_motion()) return true;

    const int x = hero_.x_tile, y = hero_.y_tile;
    const int forward_x = x + DX[direction], forward_y = y + DY[direction];
    if (contains(forward_x, forward_y) &&
        signed_byte(overlay_[static_cast<std::size_t>(forward_y) * width_ + forward_x]) == -128) {
        const auto& choices = DETOURS[direction];
        for (const auto& [side, diagonal] : {std::pair{choices.side_a, choices.diagonal_a},
                                              std::pair{choices.side_b, choices.diagonal_b}}) {
            if (passable(x + DX[side], y + DY[side]) &&
                passable(x + DX[diagonal], y + DY[diagonal])) {
                hero_.resume_direction = direction;
                hero_.motion_mode = 2;
                hero_.direction = hero_.target_direction = side;
                break;
            }
        }
    }
    hero_.movement_marker = 0;
    return tick_base_motion();
}
