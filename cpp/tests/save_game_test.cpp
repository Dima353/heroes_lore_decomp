#include "save_game.hpp"
#include <zlib.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
static void check(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
template<class F> void rejects(F f,const char* message) {bool rejected=false;try {f();} catch(const std::exception&) {rejected=true;}check(rejected,message);}
int main(int argc,char** argv) {
    try {
        {
            SaveGame done;done.character_class=8;done.switches.assign(128,0);done.quests.assign(128,0);
            done.quests[0]=0x24;done.switches[12]=0x30; // quest bits 2,5 and switches 100,101
            check(save_progress_percent(done)==4*60/16,"progress follows n.a(byte)");
            done.character_class=6;check(save_progress_percent(done)==4*60/19,"Kail divisor");
            done.new_game_plus=1;done.clear_progress=60;check(save_progress_percent(done)==60+4*30/19,"Hardcore run adds the 30% share to banked progress");
            done.new_game_plus=3;check(save_progress_percent(done)==60,"fourth run adds nothing");
        }
        SaveGame state;state.map_id=66;state.x=10;state.y=12;state.direction=3;state.quick_item_type=9;
        state.switches.assign(128,0);state.switches[0]=1;state.switches[10]=128;
        state.quests.assign(128,0);state.quests[2]=8;
        state.inventory={{0,7,1,false},{7,0,12},{9,0,3}};state.money=100;
        state.equipment[0]=InventoryStack{0,0,1};state.equipment[2]=InventoryStack{5,0,1};
        state.guardians=make_guardian_roster({true,true,false});state.guardians.equipped=1;
        state.progress.level=2;state.progress.experience=100;state.progress.attribute_points=3;
        state.vitals={35,84,20,72};
        const auto bytes=encode_save(state);const auto restored=decode_save(bytes);
        check(encode_save(restored)==bytes,"every serialized field survives round trip");
        check(restored.guardians.equipped==1 && !restored.inventory[0].identified &&
              restored.quests[2]==8 && restored.direction==3,"flags/identity/position/active guardian");
        for(std::size_t length: {std::size_t(0),std::size_t(19),bytes.size()-1}) {
            const std::vector<std::uint8_t> short_file(bytes.begin(),bytes.begin()+length);
            rejects([&]{decode_save(short_file);},"truncated save rejected");
        }
        {   // Item options are -2..8 (kOptionChance) on both write and read.
            auto odd=state;odd.inventory[0].option=100;
            rejects([&]{encode_save(odd);},"option 100 rejected on write");
            odd.inventory[0].option=8;check(decode_save(encode_save(odd)).inventory[0].option==8,"option 8 kept");
        }
        auto broken=bytes;broken.back()^=1;
        rejects([&]{decode_save(broken);},"checksum rejects bit flip");
        broken=bytes;broken[8]=6;
        rejects([&]{decode_save(broken);},"unknown version rejected");
        {   // Older versions are prefixes: v5 adds recipes, v4 n.g/ao.f, v3 widens items, v2 adds ao.z (combo).
            auto combo=state;combo.progress.max_combo=3;combo.inventory.clear();combo.equipment={};
            combo.new_game_plus=1;combo.clear_progress=40;
            const auto v4=encode_save(combo);
            const auto v4_loaded=decode_save(v4);
            check(v4_loaded.new_game_plus==1 && v4_loaded.clear_progress==40,"clear state survives round trip");
            {auto known=combo;known.recipes={0x05,0x10};check(decode_save(encode_save(known)).recipes==known.recipes,"recipe journal survives round trip");}
            const auto older=[&](std::size_t cut,std::uint32_t version) {
                std::vector<std::uint8_t> out(v4.begin(),v4.end()-std::ptrdiff_t(cut));
                const auto put=[&](std::size_t at,std::uint32_t value){for(int i=0;i<4;++i) out[at+i]=std::uint8_t(value>>(8*i));};
                put(8,version);put(12,std::uint32_t(out.size()-20));
                put(16,std::uint32_t(crc32(0,out.data()+20,static_cast<uInt>(out.size()-20))));
                return out;
            };
            const auto v4_old=decode_save(older(4,4));
            check(v4_old.recipes.empty() && v4_old.new_game_plus==1,"version 4 save loads with an empty recipe journal");
            const auto v2=decode_save(older(12,2));
            check(v2.progress.max_combo==3 && v2.new_game_plus==0 && v2.clear_progress==0,"version 2 save loads as a first run");
            check(decode_save(older(16,1)).progress.max_combo==1,"version 1 save loads with default combo");
        }
        auto invalid=state;invalid.inventory[0].count=100;
        rejects([&]{encode_save(invalid);},"invalid count rejected before writing");
        invalid=state;invalid.guardians.slots[1]=invalid.guardians.slots[0];
        rejects([&]{encode_save(invalid);},"duplicate guardian rejected");
        check(!map_allows_save(11) && !map_allows_save(13) && !map_allows_save(15) &&
              !map_allows_save(82) && map_allows_save(66),"original boss map restriction");
        if(argc>1) {
            const std::filesystem::path root(argv[1]);std::filesystem::create_directories(root);
            const auto file=(root/"test-slot.hls").string();
            write_save(file,state);state.money=200;write_save(file,state);
            bool backup=true;check(read_save(file,&backup).money==200 && !backup,"new slot loads");
            {std::ofstream f(file,std::ios::binary|std::ios::trunc);f<<"broken";}
            check(read_save(file,&backup).money==100 && backup,"corrupt primary falls back to previous complete save");
            state.money=300;write_save(file,state);
            check(read_save(file).money==300,"saving recovers corrupt primary");
            check(read_save(file+".bak").money==100,"good backup retained after corrupt primary");
            rejects([&]{write_save((root/"missing"/"slot.hls").string(),state);},"write failure reported");
            check(read_save(file).money==300,"failed write does not damage current slot");
        }
        std::cout<<"Save game checks passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
