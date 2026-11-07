#pragma once
#include "event_trigger.hpp"
#include "hero_vitals.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
// g.a: cycle these four categories; the first stack of that type is selected.
constexpr bool is_quick_item(int type) { return type>=7 && type<=10; }
inline std::optional<std::size_t> quick_item_slot(const std::vector<InventoryStack>& bag,int type) {
    for(std::size_t i=0;i<bag.size();++i)
        if(bag[i].item_type==type && bag[i].count>0) return i;
    return std::nullopt;
}
enum class ItemUseResult { used, invalid_slot, unsupported };
// antidote (type 10) is always spent; *antidote reports it so the caller clears poison (cf 7).
inline ItemUseResult use_consumable(std::vector<InventoryStack>& bag,std::size_t slot,HeroVitals& vitals,
                                    bool* antidote=nullptr) {
    if(slot>=bag.size() || bag[slot].count<=0) return ItemUseResult::invalid_slot;
    const int type=bag[slot].item_type;
    // ao.a(ad): 7/8 -> HP percent; 9 -> SP percent; 10 removes poison.
    if(type<7 || type>10) return ItemUseResult::unsupported;
    if(antidote) *antidote=type==10;
    if(type==9) vitals.sp=std::min(vitals.max_sp,vitals.sp+int(std::int64_t(vitals.max_sp)*30/100));
    else if(type!=10) vitals.hp=std::min(vitals.max_hp,vitals.hp+int(std::int64_t(vitals.max_hp)*(type==7?20:40)/100));
    if(--bag[slot].count==0) bag.erase(bag.begin()+static_cast<std::ptrdiff_t>(slot));
    return ItemUseResult::used;
}
