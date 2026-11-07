#pragma once
#include "hero_vitals.hpp"
#include "guardian.hpp"
#include <array>
#include <cstdint>
#include <algorithm>

struct HeroProgress {
    int level{1},experience{},attribute_points{};
    std::array<int,4> attributes{{8,5,3,4}}; // Power, HP attribute, Agility, Will (ao.b/e/f/g).
    int max_combo{1}; // ao.z: combo steps unlocked; COMBO (opcode 19) raises it via ao.d(byte).
    std::array<int,4> gear_bonus{}; // ao.m..p: equipped j[] sums (derived, not saved)
    int next_experience() const {return level*level*level-level*level+80*level;}
};
inline void recalculate_vitals(const HeroProgress& hero,HeroVitals& vitals) {
    // ao.n: d = (e + n + level) * 12, e = (g + p + level) * 12.
    vitals.max_hp=(hero.attributes[1]+hero.gear_bonus[1]+hero.level)*12;
    vitals.max_sp=(hero.attributes[3]+hero.gear_bonus[3]+hero.level)*12;
}
inline void add_event_experience(HeroProgress& hero,HeroVitals& vitals,GuardianRoster& roster,unsigned reward) {
    const int amount=int(reward)*4; // ah.o -> ao.f(int), including active guardian.
    hero.experience+=amount;
    while(hero.experience>=hero.next_experience()) {
        hero.experience-=hero.next_experience();
        if(hero.level<99) {
            ++hero.level;hero.attribute_points+=3;
            recalculate_vitals(hero,vitals);
        }
        vitals.hp=vitals.max_hp;vitals.sp=vitals.max_sp;
    }
    if(roster.equipped<roster.slots.size()) if(auto& active=roster.slots[roster.equipped]) {
        active->experience+=amount;
        while(active->experience>=active->next_experience()) {
            active->experience-=active->next_experience();
            ++active->level;
            if(active->level==9) active->wild=1; // p.f unlocks skill index 1 at LV9.
        }
    }
}

inline int attribute_points_remaining(const HeroProgress& hero,const std::array<int,4>& pending) {
    int remaining=hero.attribute_points;
    for(int value:pending) {if(value<0 || value>remaining) return -1;remaining-=value;}
    return remaining;
}
inline bool apply_attribute_points(HeroProgress& hero,HeroVitals& vitals,const std::array<int,4>& pending) {
    const int remaining=attribute_points_remaining(hero,pending);
    if(remaining<0) return false;
    for(std::size_t i=0;i<4;++i) hero.attributes[i]+=pending[i];
    hero.attribute_points=remaining;
    recalculate_vitals(hero,vitals); // bi commits through ao.n; no free heal.
    vitals.hp=std::min(vitals.hp,vitals.max_hp);vitals.sp=std::min(vitals.sp,vitals.max_sp);
    return true;
}
