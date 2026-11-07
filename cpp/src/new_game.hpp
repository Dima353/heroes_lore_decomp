#pragma once
#include "equipment.hpp"
#include <array>
#include <optional>

struct NewGameProfile {
    int character_class{};
    int map_id{};
    int x{}, y{};
    int direction{2};
    std::int64_t money{300};
    std::array<int, 4> base_attributes{}; // ao.b/e/f/g: Power, HP attribute, Agility, Will.
    // ao equipment slots: weapon, shield, armor, head, accessory. Not bag slots.
    EquipmentSet equipment;
    // Default port character setup: Hestia + Ran; original accepts three choice flags.
    std::array<bool,3> guardian_choices{{true,true,false}};
    std::vector<std::uint8_t> switches;
    std::vector<std::uint8_t> quests;
};
NewGameProfile make_new_game_profile(int character_class);
