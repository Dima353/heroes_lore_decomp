#pragma once
#include "equipment.hpp"
#include "hero_progress.hpp"
#include <string>
struct SaveGame {
    int character_class{6},map_id{},zone{6},x{},y{},direction{2},quick_item_type{7};
    std::int64_t money{300};
    std::vector<std::uint8_t> switches,quests;
    std::vector<InventoryStack> inventory;
    EquipmentSet equipment;
    HeroVitals vitals;
    HeroProgress progress;
    GuardianRoster guardians;
    int new_game_plus{};   // n.g: 1 after the first ending ("Hardcore mode"), picks enm/data<g>
    int clear_progress{};  // ao.f: progress % banked by earlier clears (capped at 100)
    std::vector<std::uint8_t> recipes; // port: recipe_journal.hpp slots the hero has seen (version 5)
};
bool map_allows_save(int map_id);
// n.a(byte): quest bits 2+3i (i<20) plus switches 100..105, scaled by the run's share
// n.h[g] = {60,30,10} over the hero's quest count (19/21/16); nothing from the 4th run on.
int run_progress_percent(const SaveGame& game);
// n.p: banked ao.f plus the current run.
int save_progress_percent(const SaveGame& game);
std::vector<std::uint8_t> encode_save(const SaveGame& game);
SaveGame decode_save(const std::vector<std::uint8_t>& bytes);
SaveGame read_save(const std::string& path,bool* used_backup=nullptr);
void write_save(const std::string& path,const SaveGame& game);
