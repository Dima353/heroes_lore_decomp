#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>
struct QuestJournalEntry {
    int id{};bool main_quest{};
    std::string name,description,location,giver,reward;
};
// n.a[[Z and s.<init>/d: 15 entries per class; bits start/hidden/second phase.
inline std::vector<QuestJournalEntry> build_quest_journal(int character_class,
    const std::vector<std::string>& text,const std::vector<std::uint8_t>& bits) {
    static constexpr bool main[3][15]={
        {1,1,1,1,1,1,0,0,0,0,0,0,0,0,0},
        {1,0,1,0,0,0,0,1,1,1,1,0,0,1,1},
        {1,1,1,1,0,0,0,0,0,0,0,0,0,0,0}};
    if(character_class<6 || character_class>8 || text.size()!=105)
        throw std::runtime_error("invalid quest journal table");
    const auto set=[&](unsigned id){return id/8<bits.size() && ((bits[id/8]>>(id%8))&1u);};
    std::vector<QuestJournalEntry> result;
    for(int i=0;i<15;++i) {
        const unsigned start=1+3*i;
        if(set(start+1) || !set(start)) continue;
        const auto base=std::size_t(i)*7;
        const auto phase=base+(set(start+2)?2:0);
        if(text[phase].empty()) continue;
        result.push_back({i,main[character_class-6][i],text[phase],text[phase+1],
                          text[base+4],text[base+5],text[base+6]});
    }
    return result;
}
