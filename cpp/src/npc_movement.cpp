#include "npc_movement.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace {
constexpr std::array<int, 9> DX{{0, 0, 0, -1, 1, -1, 1, -1, 1}};
constexpr std::array<int, 9> DY{{0, -1, 1, 0, 0, -1, -1, 1, 1}};

std::int16_t wrap_short(int value) {
    const auto bits = static_cast<std::uint16_t>(value);
    return static_cast<std::int16_t>(bits >= 0x8000
        ? static_cast<int>(bits) - 0x10000 : bits);
}

int floor_tile(int pixels) {
    return pixels >= 0 ? pixels / 16 : -((-pixels + 15) / 16);
}

std::int8_t wrap_byte(int value) {
    const auto bits = static_cast<std::uint8_t>(value);
    return static_cast<std::int8_t>(bits >= 128
        ? static_cast<int>(bits) - 256 : bits);
}

void sync_position(NpcMotionState& npc) {
    npc.x_tile = wrap_byte(floor_tile(npc.x_pixels));
    npc.y_tile = wrap_byte(floor_tile(npc.y_pixels));
    const auto x_remainder = npc.x_pixels % 16;
    const auto y_remainder = npc.y_pixels % 16;
    npc.partial_x = x_remainder != 0;
    npc.partial_y = y_remainder != 0;
}

int signed_byte(std::uint8_t value) {
    return value >= 128 ? static_cast<int>(value) - 256 : value;
}

void mark_footprint(std::vector<int>& occupancy, int width, int height,
                    int owner, int x, int y, bool partial_x, bool partial_y) {
    const auto mark = [&](int cell_x, int cell_y) {
        if (cell_x >= 0 && cell_y >= 0 && cell_x < width && cell_y < height)
            occupancy[static_cast<std::size_t>(cell_y) * width + cell_x] = owner;
    };
    mark(x, y);
    if (partial_y) mark(x, y + 1);
    else if (partial_x) mark(x + 1, y);
}
}

std::vector<NpcMotionState> make_npc_motion_states(const EventGrid& event) {
    if (event.npcs.size() % 3 != 0)
        throw std::invalid_argument("NPC table is not 3-byte aligned");
    std::vector<NpcMotionState> result;
    result.reserve(event.npcs.size() / 3);
    for (std::size_t i = 0; i < event.npcs.size(); i += 3) {
        NpcMotionState npc;
        npc.x_pixels = wrap_short(static_cast<int>(event.npcs[i]) * 16);
        npc.y_pixels = wrap_short(static_cast<int>(event.npcs[i + 1]) * 16);
        sync_position(npc);
        result.push_back(npc);
    }
    return result;
}

void apply_npc_movement_command(const EventCommand& command,
                                std::vector<NpcMotionState>& npcs) {
    if (command.opcode < 15 || command.opcode > 17)
        throw std::invalid_argument("command is not MV_N_MOV/MV_N_STP/MV_N_DIR");
    const auto signed_index = static_cast<int>(command.arg1 >= 128
        ? static_cast<int>(command.arg1) - 256 : command.arg1);
    if (signed_index < 0 || static_cast<std::size_t>(signed_index) >= npcs.size())
        throw std::out_of_range("NPC movement command references missing NPC");
    auto& npc = npcs[static_cast<std::size_t>(signed_index)];
    if (command.opcode == 15 || command.opcode == 16) {
        npc.motion_mode = command.opcode == 15 ? 2 : 1;
        npc.movement_marker = -1;
        npc.animation_frame = 0;
    }
    if (command.opcode == 15 || command.opcode == 17) {
        const auto direction = static_cast<std::int8_t>(command.arg2 >= 128
            ? static_cast<int>(command.arg2) - 256 : command.arg2);
        if (direction < 0 || direction > 8)
            throw std::out_of_range("NPC movement direction outside 0..8");
        npc.direction = static_cast<std::uint8_t>(direction);
        npc.target_direction = npc.direction;
    }
}

std::vector<std::size_t> tick_npc_movement(
    std::vector<NpcMotionState>& npcs, const GameMap& map, const EventGrid& event,
    const std::vector<MapEntity>& enemies, const HeroState& hero) {
    const int width = map.width;
    const int height = map.height;
    const auto cell_count = static_cast<std::size_t>(width) * height;
    if (width <= 0 || height <= 0 || event.overlay.size() != cell_count)
        throw std::invalid_argument("NPC movement requires a valid map collision grid");

    // ae registers the hero, visible NPCs and enemies in one occupancy grid.
    // Actor width is one tile (ck.e defaults to 1); a half-step occupies both
    // cells touched by the actor, matching o.f()/o.g().
    constexpr int kEmpty = -1;
    std::vector<int> occupancy(cell_count, kEmpty);
    for (std::size_t i = 0; i < npcs.size(); ++i) {
        if (i < event.npc_visible.size() && !event.npc_visible[i]) continue;
        const auto& npc = npcs[i];
        mark_footprint(occupancy, width, height, static_cast<int>(i),
                       npc.x_tile, npc.y_tile, npc.partial_x, npc.partial_y);
    }
    const int hero_owner = static_cast<int>(npcs.size());
    mark_footprint(occupancy, width, height, hero_owner,
                   hero.x_tile, hero.y_tile, hero.partial_x, hero.partial_y);
    for (std::size_t i = 0; i < enemies.size(); ++i) {
        const auto& enemy = enemies[i];
        mark_footprint(occupancy, width, height,
                       hero_owner + 1 + static_cast<int>(i),
                       enemy.x, enemy.y, enemy.partial_x, enemy.partial_y);
    }

    std::vector<std::size_t> moved;
    for (std::size_t i = 0; i < npcs.size(); ++i) {
        auto& npc = npcs[i];
        if (npc.motion_mode != 2 && npc.motion_mode != 4) continue;
        if (npc.direction >= DX.size())
            throw std::out_of_range("NPC has invalid movement direction");

        const bool visible = i >= event.npc_visible.size() || event.npc_visible[i];
        if (!npc.partial_x && !npc.partial_y) {
            const int target_x = int(npc.x_tile) + DX[npc.direction];
            const int target_y = int(npc.y_tile) + DY[npc.direction];
            const bool inside = target_x >= 0 && target_y >= 0 &&
                                target_x < width && target_y < height;
            bool blocked = !inside;
            if (inside) {
                const auto target_index =
                    static_cast<std::size_t>(target_y) * width + target_x;
                const int occupant = occupancy[target_index];
                blocked = (signed_byte(event.overlay[target_index]) < 0 ||
                           occupant != kEmpty) &&
                          occupant != static_cast<int>(i);
            }
            if (blocked) {
                npc.motion_mode = 1;
                npc.movement_marker = -1;
                npc.animation_frame = 0;
                continue;
            }
        }

        if (visible) {
            const auto clear = [&](int x, int y) {
                if (x < 0 || y < 0 || x >= width || y >= height) return;
                auto& cell = occupancy[static_cast<std::size_t>(y) * width + x];
                if (cell == static_cast<int>(i)) cell = kEmpty;
            };
            clear(npc.x_tile, npc.y_tile);
            if (npc.partial_y) clear(npc.x_tile, int(npc.y_tile) + 1);
            else if (npc.partial_x) clear(int(npc.x_tile) + 1, npc.y_tile);
        }

        npc.x_pixels = wrap_short(npc.x_pixels + 8 * DX[npc.direction]);
        npc.y_pixels = wrap_short(npc.y_pixels + 8 * DY[npc.direction]);
        sync_position(npc);
        if (visible)
            mark_footprint(occupancy, width, height, static_cast<int>(i),
                           npc.x_tile, npc.y_tile,
                           npc.partial_x, npc.partial_y);
        moved.push_back(i);
    }
    return moved;
}
