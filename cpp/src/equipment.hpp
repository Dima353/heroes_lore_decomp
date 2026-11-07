#pragma once
#include "event_trigger.hpp"
#include "item_catalog.hpp"
#include <algorithm>
#include <array>
#include <optional>

using EquipmentSet = std::array<std::optional<InventoryStack>, 5>;
struct EquipmentInfo {
    std::uint8_t power{}; // e.a: base attack for weapons, defense for other gear.
    std::int8_t secondary{}; // e.d: refinement limit (at rejects e.e >= e.d).
    bool flag{}; // e.a boolean; meaning not inferred.
    std::int8_t random_option_count{};
    std::array<std::array<std::int8_t, 2>, 4> option_ranges{};
    std::vector<std::uint8_t> subtype_data;
};
std::optional<EquipmentInfo> equipment_info(const ShopItem& item);
struct WeaponProperties {int critical_chance{},critical_bonus_tenths{},hit_option{-1};};
std::optional<WeaponProperties> weapon_properties(const ShopItem& item);
const ShopItem* find_catalog_item(const std::vector<ShopItem>& catalog, int type, int id);
// t.c of a weapon (class_data[14]) or shield ([12]); the instance value once the refiner set it.
int item_option(const InventoryStack& stack, const ShopItem* item);
inline bool has_option_ranges(const EquipmentInfo& info) {
    for (const auto& range : info.option_ranges) if (range[0] || range[1]) return true;
    return false;
}
// ad.a(type,id,true,randomize): gear without bonus ranges is identified; with `randomize`
// e.a(byte,byte[],byte[],byte) rolls `count` distinct bonuses inside the ranges.
// roll(lo,hi) is h.a(int,int), inclusive.
template<class Roll> void roll_equipment(InventoryStack& stack, const ShopItem& item, bool randomize, Roll roll) {
    const auto info = equipment_info(item);
    if (!info) return;
    const bool ranged = has_option_ranges(*info);
    stack.identified = !ranged;
    stack.refine = 0;
    stack.bonus = {};
    if (!randomize || !ranged) return;
    int usable = 0;
    for (const auto& range : info->option_ranges) usable += range[0] || range[1];
    for (int n = 0; n < std::min<int>(info->random_option_count, usable); ++n) {
        int v;
        do v = roll(0, 3);
        while (stack.bonus[std::size_t(v)] != 0 ||
               (!info->option_ranges[std::size_t(v)][0] && !info->option_ranges[std::size_t(v)][1]));
        const auto& range = info->option_ranges[std::size_t(v)];
        stack.bonus[std::size_t(v)] = static_cast<std::int8_t>(roll(std::min(range[0], range[1]), std::max(range[0], range[1])));
    }
}
// ao.n: weapon a+e*5/2, shield a+e, armour a+2e (its e also adds to hit), helmet/boots a;
// the j[] bonuses of every equipped piece are summed into m/n/o/p.
struct GearTotals { int weapon{}, defense{}, armour_refine{}; std::array<int,4> bonus{}; };
GearTotals gear_totals(const EquipmentSet& equipment, const std::vector<ShopItem>& catalog);
int equipment_slot(std::uint8_t type) noexcept;
enum class EquipResult { equipped, invalid_slot, not_equipment, wrong_class, unidentified, invalid_item };
EquipResult equip_bag_slot(int character_class, std::size_t slot,
    const std::vector<ShopItem>& catalog, std::vector<InventoryStack>& bag, EquipmentSet& equipment);
struct EquipmentVisualLayer { std::string category, atlas; bool enabled{}; };
std::array<EquipmentVisualLayer, 6> equipment_visual_layers(int character_class, const EquipmentSet& equipment);
