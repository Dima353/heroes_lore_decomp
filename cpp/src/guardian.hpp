#pragma once
#include <array>
#include <optional>
#include <stdexcept>

struct Guardian {
    int id{},level{1},experience{},calm{0},wild{-1};
    int next_experience() const { return level*level*level-level*level+80*level; }
};
struct GuardianRoster {
    std::array<std::optional<Guardian>,5> slots;
    std::size_t equipped{};
};
inline bool award_guardian(GuardianRoster& roster,int id) {
    if(id<0 || id>5) throw std::invalid_argument("invalid guardian ID");
    for(const auto& slot:roster.slots) if(slot && slot->id==id) return false;
    for(auto& slot:roster.slots) if(!slot) {
        const int level=id==4?10:id==5?20:1; // p.a static starting levels.
        slot=Guardian{id,level,0,0,level>=9?1:-1};return true;
    }
    return false;
}
// n.a(false,class,choices): the character setup grants the selected calm guardians.
inline GuardianRoster make_guardian_roster(const std::array<bool,3>& choices) {
    GuardianRoster roster;
    for(int i=0;i<3;++i) if(choices[i]) award_guardian(roster,i);
    return roster;
}
inline void award_event_guardian(GuardianRoster& roster,unsigned choice) {
    static constexpr int ids[]{4,3,5}; // ah.r, opcode 18.
    if(choice>2) throw std::invalid_argument("invalid GUARDIAN event choice");
    award_guardian(roster,ids[choice]);
}
