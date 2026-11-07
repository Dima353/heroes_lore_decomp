#include "new_game.hpp"
#include <stdexcept>

NewGameProfile make_new_game_profile(int character_class) {
    if (character_class < 6 || character_class > 8)
        throw std::invalid_argument("new game class must be 6..8");
    // n.j triples: map, x, y, indexed by class-6. bu.c uses direction index 1.
    static constexpr int starts[3][3]{{0,22,4},{60,5,36},{77,10,18}};
    static constexpr int stats[3][4]{{8,5,3,4},{3,4,8,5},{5,8,4,3}};
    static constexpr std::int8_t weapons[3]{0,2,1};
    const auto index = character_class - 6;
    NewGameProfile profile;
    profile.character_class = character_class;
    profile.map_id = starts[index][0];
    profile.x = starts[index][1]; profile.y = starts[index][2];
    for (int i = 0; i < 4; ++i) profile.base_attributes[i] = stats[index][i];
    profile.equipment[0] = InventoryStack{weapons[index],0,1};
    if (character_class == 8) profile.equipment[1] = InventoryStack{3,0,1};
    profile.equipment[2] = InventoryStack{5,0,1};
    profile.equipment[3] = InventoryStack{6,0,1};
    profile.equipment[4] = InventoryStack{4,0,1};
    profile.switches.assign(128,0); profile.switches[0] = 1;
    profile.quests.assign(128,0);
    return profile;
}
