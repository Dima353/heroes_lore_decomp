#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Port: recipes the hero has been told. The story shows them as dialogue lines tagged
// "[Combination]" / "[Рецепт]"; these are their language-bank indices, identical in the
// English and Russian JARs. The original keeps no record of them.
inline constexpr std::array<int,21> kRecipeTexts{
    1856,1881,1965,2220,2231,2278,2280,2566,2652,2709,2774,2894,3098,3099,3210,3413,3481,3577,3658,3659,3741};

// Slot of a bank index in kRecipeTexts, or -1.
inline int recipe_slot(std::size_t bank_index) {
    for(std::size_t i=0;i<kRecipeTexts.size();++i) if(std::size_t(kRecipeTexts[i])==bank_index) return int(i);
    return -1;
}
inline bool recipe_known(const std::vector<std::uint8_t>& bits,int slot) {
    return slot>=0 && std::size_t(slot/8)<bits.size() && ((bits[std::size_t(slot/8)]>>(slot%8))&1u);
}
inline void mark_recipe(std::vector<std::uint8_t>& bits,int slot) {
    if(slot<0) return;
    if(bits.size()<(kRecipeTexts.size()+7)/8) bits.resize((kRecipeTexts.size()+7)/8);
    bits[std::size_t(slot/8)]|=std::uint8_t(1u<<(slot%8));
}

// "[tag] Result = part + part" split into the result and its parts (trimmed).
struct RecipeEntry { std::string result; std::vector<std::string> parts; };
inline RecipeEntry parse_recipe_text(const std::string& text) {
    const auto trim=[](std::string s){
        while(!s.empty() && (s.front()==' ' || s.front()=='|' || s.front()=='$')) s.erase(s.begin());
        while(!s.empty() && (s.back()==' ' || s.back()=='\n')) s.pop_back();
        return s;};
    std::string body=text;
    if(const auto close=body.find(']');body.find('[')!=std::string::npos && close!=std::string::npos) body=body.substr(close+1);
    RecipeEntry entry;
    const auto eq=body.find('=');
    entry.result=trim(body.substr(0,eq));
    if(eq==std::string::npos) return entry;
    std::string rest=body.substr(eq+1);
    for(std::size_t start=0;start<=rest.size();) {
        const auto plus=std::min(rest.find('+',start),rest.size());
        if(auto part=trim(rest.substr(start,plus-start));!part.empty()) entry.parts.push_back(part);
        start=plus+1;
    }
    return entry;
}
