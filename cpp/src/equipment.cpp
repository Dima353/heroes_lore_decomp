#include "equipment.hpp"
#include <stdexcept>
#include <utility>

namespace {
#include "equipment_visual_tables.inc"
std::int8_t signed_byte(std::uint8_t value) {
    return static_cast<std::int8_t>(value < 128 ? int(value) : int(value)-256);
}
template<std::size_t N> int variant(const int (&table)[N], const std::optional<InventoryStack>& gear) {
    if (!gear) return -1;
    const int id = gear->item_id;
    if (id < 0 || id >= static_cast<int>(N)) throw std::out_of_range("equipment visual ID out of range");
    return table[id];
}
}
const ShopItem* find_catalog_item(const std::vector<ShopItem>& catalog, int type, int id) {
    for (const auto& item : catalog) if (item.type == type && item.id == id) return &item;
    return nullptr;
}
int item_option(const InventoryStack& stack, const ShopItem* item) {
    if (stack.option != -2) return stack.option;
    if (!item || item->type > 3) return -1;
    const auto index = item->type == 3 ? 12u : 14u;
    return index < item->class_data.size() ? signed_byte(item->class_data[index]) : -1;
}
GearTotals gear_totals(const EquipmentSet& equipment, const std::vector<ShopItem>& catalog) {
    GearTotals totals;
    for (std::size_t slot = 0; slot < equipment.size(); ++slot) if (const auto& gear = equipment[slot]) {
        for (std::size_t i = 0; i < 4; ++i) totals.bonus[i] += gear->bonus[i];
        const auto* item = find_catalog_item(catalog, gear->item_type, gear->item_id);
        const auto info = item ? equipment_info(*item) : std::nullopt;
        const int power = info ? info->power : 0, e = gear->refine;
        if (slot == 0) totals.weapon = power + e * 5 / 2;
        else totals.defense += power + (slot == 1 ? e : slot == 2 ? e * 2 : 0);
        if (slot == 2) totals.armour_refine = e;
    }
    return totals;
}
int equipment_slot(std::uint8_t type) noexcept {
    if (type <= 2) return 0;
    switch (type) { case 3: return 1; case 5: return 2; case 6: return 3; case 4: return 4; default: return -1; }
}
std::optional<EquipmentInfo> equipment_info(const ShopItem& item) {
    if (equipment_slot(item.type) < 0) return std::nullopt;
    const auto required = item.type <= 2 ? 15u : item.type == 3 ? 13u : 12u;
    if (item.class_data.size() < required) throw std::runtime_error("truncated equipment data");
    EquipmentInfo info;
    info.power = item.class_data[0];
    info.secondary = signed_byte(item.class_data[1]);
    info.flag = item.class_data[2] != 0;
    info.random_option_count = signed_byte(item.class_data[3]);
    for (std::size_t i = 0; i < 4; ++i) for (std::size_t j = 0; j < 2; ++j)
        info.option_ranges[i][j] = signed_byte(item.class_data[4 + i*2 + j]);
    info.subtype_data.assign(item.class_data.begin()+12, item.class_data.end());
    return info;
}
std::optional<WeaponProperties> weapon_properties(const ShopItem& item) {
    if(item.type>2) return std::nullopt;
    const auto info=equipment_info(item);
    // l.a(boolean,byte[],int): three signed bytes after the e payload.
    return WeaponProperties{signed_byte(info->subtype_data[0]),signed_byte(info->subtype_data[1]),signed_byte(info->subtype_data[2])};
}
EquipResult equip_bag_slot(int character_class, std::size_t slot,
    const std::vector<ShopItem>& catalog, std::vector<InventoryStack>& bag, EquipmentSet& equipment) {
    if (slot >= bag.size()) return EquipResult::invalid_slot;
    const auto chosen = bag[slot];
    const auto dest = equipment_slot(static_cast<std::uint8_t>(chosen.item_type));
    if (dest < 0) return EquipResult::not_equipment;
    if (character_class < 6 || character_class > 8) return EquipResult::wrong_class;
    const int weapon_type[] = {0,2,1};
    if ((dest == 0 && chosen.item_type != weapon_type[character_class-6]) ||
        (dest == 1 && character_class != 8)) return EquipResult::wrong_class;
    const auto* item = find_catalog_item(catalog, chosen.item_type, chosen.item_id);
    if (!item || chosen.count != 1) return EquipResult::invalid_item;
    if (!equipment_info(*item)) return EquipResult::not_equipment;
    if (!chosen.identified) return EquipResult::unidentified;
    // g.a(ad,byte): swap the selected bag slot with the equipped object.
    if (equipment[dest]) bag[slot] = *equipment[dest];
    else bag.erase(bag.begin() + static_cast<std::ptrdiff_t>(slot));
    equipment[dest] = chosen;
    return EquipResult::equipped;
}
std::array<EquipmentVisualLayer, 6> equipment_visual_layers(int character_class, const EquipmentSet& equipment) {
    if (character_class < 6 || character_class > 8) throw std::invalid_argument("invalid equipment class");
    const int c = character_class - 6;
    const int w = variant(weapon_variants[c], equipment[0]);
    const int a = variant(armor_variants[c], equipment[2]);
    const int h = variant(head_variants, equipment[3]);
    const int sh = variant(shield_variants, equipment[1]);
    const auto atlas = [](const char* prefix, int value) {
        return value < 0 ? std::string{} : prefix + std::to_string(value + 1);
    };
    const bool head_a = character_class == 6 && (!equipment[3] || equipment[3]->item_id <= 3);
    return {{{"b","b",true}, {head_a ? "hA" : "hB",atlas("h",h < 0 ? 0 : h),true},
             {"a",atlas("a",a),a >= 0}, {"w",atlas("w",w),w >= 0},
             {"s",atlas("s",sh),character_class == 8 && sh >= 0},
             {"e","e",true}}}; // bu category 2: weapon swing trails (frame layer 7)
}
