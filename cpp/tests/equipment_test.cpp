#include "equipment.hpp"
#include "new_game.hpp"
#include <iostream>
#include <stdexcept>

static void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static ShopItem gear(int type, int id, int power) {
    ShopItem item;
    item.type = static_cast<std::uint8_t>(type); item.id = static_cast<std::uint8_t>(id);
    item.class_data.assign(type <= 2 ? 15 : type == 3 ? 13 : 12, 0);
    item.class_data[0] = static_cast<std::uint8_t>(power);
    return item;
}
int main() {
    try {
        std::vector<ShopItem> catalog{gear(0,7,25),gear(2,1,12),gear(3,0,2),gear(5,1,3)};
        auto equipment = make_new_game_profile(6).equipment;
        std::vector<InventoryStack> bag(30, {7,0,1}); bag[12] = {0,7,1};
        check(equip_bag_slot(6,12,catalog,bag,equipment) == EquipResult::equipped, "swap works with full bag");
        check(bag.size() == 30 && bag[12].item_type == 0 && bag[12].item_id == 0 &&
              equipment[0]->item_id == 7 && bag[0].item_type == 7, "only selected slot exchanged");
        const auto visuals = equipment_visual_layers(6,equipment);
        check(visuals[3].atlas == "w2" && !visuals[2].enabled && visuals[1].atlas == "h1",
              "weapon variant and absent starter armor");
        bag[12] = {2,1,1};
        check(equip_bag_slot(6,12,catalog,bag,equipment) == EquipResult::wrong_class &&
              equipment[0]->item_id == 7 && bag[12].item_type == 2, "wrong class leaves state unchanged");
        bag[12] = {3,0,1};
        check(equip_bag_slot(6,12,catalog,bag,equipment) == EquipResult::wrong_class, "shield requires class 8");
        bag[12] = {0,7,1,false};
        check(equip_bag_slot(6,12,catalog,bag,equipment) == EquipResult::unidentified &&
              !bag[12].identified, "unidentified gear rejected atomically");
        check(equip_bag_slot(6,0,catalog,bag,equipment) == EquipResult::not_equipment, "consumable not equipment");
        check(equip_bag_slot(6,99,catalog,bag,equipment) == EquipResult::invalid_slot, "bad slot");
        equipment[2].reset(); bag = {{5,1,1}};
        check(equip_bag_slot(6,0,catalog,bag,equipment) == EquipResult::equipped && bag.empty() &&
              equipment[2]->item_id == 1, "empty equipment slot consumes bag entry");
        const auto info = equipment_info(catalog[0]);
        check(info && info->power == 25 && info->subtype_data.size() == 3, "weapon data layout");
        auto weapon=gear(0,0,25);weapon.class_data[12]=7;weapon.class_data[13]=3;weapon.class_data[14]=255;
        const auto properties=weapon_properties(weapon);
        check(properties && properties->critical_chance==7 && properties->critical_bonus_tenths==3 &&
              properties->hit_option==-1,"l fields preserve crit chance, bonus tenths and signed option sentinel");
        check(!weapon_properties(gear(5,0,3)),"armor does not expose weapon options");
        auto broken = gear(0,1,1); broken.class_data.resize(14);
        bool rejected = false;
        try { equipment_info(broken); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected, "short weapon data rejected");
        std::cout << "Equipment checks passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
