#include "save_game.hpp"
#include <zlib.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <algorithm>
namespace {
using Bytes=std::vector<std::uint8_t>;
void valid(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
void validate(const SaveGame& s) {
    valid(s.character_class>=6 && s.character_class<=8 && s.map_id>=0 && s.map_id<=99 &&
          s.zone>=6 && s.zone<=8 && s.x>=0 && s.x<128 && s.y>=0 && s.y<128 &&
          s.direction>=1 && s.direction<=4,"invalid saved position/class");
    valid(s.money>=0 && s.money<=2147483647 && s.quick_item_type>=7 && s.quick_item_type<=10,"invalid saved money/item");
    valid(s.switches.size()==128 && s.quests.size()==128 && s.inventory.size()<=30,"invalid saved bank size");
    // Same limits as Reader::item: refine 0..127, option -2 (catalog), -1 (none) or 0..8 (kOptionChance).
    const auto item=[](const InventoryStack& v){return v.item_type>=0 && v.item_type<=23 && v.item_id>=0 && v.count>0 && v.count<=99 &&
                                                       v.refine>=0 && v.option>=-2 && v.option<=8;};
    for(const auto& v:s.inventory) valid(item(v),"invalid saved inventory stack");
    for(std::size_t i=0;i<s.equipment.size();++i) if(const auto& v=s.equipment[i]) {
        valid(item(*v) && v->count==1,"invalid saved equipment stack");
        const int type=v->item_type;
        valid(i==0?type<=2:i==1?type==3:i==2?type==5:i==3?type==6:type==4,"invalid saved equipment slot");
    }
    valid(s.progress.level>=1 && s.progress.level<=99 && s.progress.experience>=0 &&
          s.progress.experience<s.progress.next_experience() && s.progress.attribute_points>=0 &&
          s.progress.attribute_points<=32767,"invalid saved progression");
    for(int attr:s.progress.attributes) valid(attr>=0 && attr<=32767,"invalid saved attribute");
    valid(s.progress.max_combo>=1 && s.progress.max_combo<=5,"invalid saved combo length");
    valid(s.new_game_plus>=0 && s.new_game_plus<=127 && s.clear_progress>=0 && s.clear_progress<=100,"invalid saved clear state");
    valid(s.vitals.max_hp>0 && s.vitals.max_hp<=1000000 && s.vitals.max_sp>0 && s.vitals.max_sp<=1000000 &&
          s.vitals.hp>=0 && s.vitals.hp<=s.vitals.max_hp && s.vitals.sp>=0 && s.vitals.sp<=s.vitals.max_sp,"invalid saved HP/SP");
    valid(s.guardians.equipped<5 && bool(s.guardians.slots[s.guardians.equipped]),"invalid active guardian");
    unsigned seen=0;
    for(const auto& g:s.guardians.slots) if(g) {
        valid(g->id>=0 && g->id<6 && !(seen&(1u<<g->id)),"invalid/duplicate saved guardian");seen|=1u<<g->id;
        valid(g->level>=1 && g->level<=1000 && g->experience>=0 && g->experience<g->next_experience() &&
              g->calm>=-1 && g->calm<=3 && g->wild>=-1 && g->wild<=3,"invalid saved guardian stats");
    }
}
void u32(Bytes& b,std::uint32_t n) {for(int i=0;i<4;++i) b.push_back(std::uint8_t(n>>(i*8)));}
struct Reader {
    const Bytes& b;std::size_t p{};
    std::uint32_t number() {valid(p<=b.size() && b.size()-p>=4,"truncated save");std::uint32_t n=0;
        for(int i=0;i<4;++i) n|=std::uint32_t(b[p++])<<(i*8);
        return n;}
    int integer() {return static_cast<std::int32_t>(number());}
    Bytes bank() {const auto size=number();valid(size==128 && b.size()-p>=size,"invalid save flag bank");
        Bytes out(b.begin()+p,b.begin()+p+size);p+=size;return out;}
    std::uint32_t version{1};
    InventoryStack item() {const int type=integer(),id=integer(),count=integer(),known=integer();
        valid(type>=0 && type<=23 && id>=0 && id<=127 && (known==0 || known==1),"invalid item encoding");
        InventoryStack v{static_cast<std::int8_t>(type),static_cast<std::int8_t>(id),count,known!=0};
        if(version>=3) { // ad.a() bytes 4..9; v1/v2 saves keep the defaults.
            const auto byte=[&](int low){const int n=integer();valid(n>=low && n<=127,"invalid item instance");return static_cast<std::int8_t>(n);};
            v.refine=byte(0);for(auto& b:v.bonus) b=byte(-128);v.option=byte(-2);
            valid(v.option<=8,"invalid item option");
        }
        return v;}
};
Bytes read_bytes(const std::string& path) {
    std::ifstream f(path,std::ios::binary|std::ios::ate);valid(bool(f),"save file not found");
    const auto size=f.tellg();valid(size>=20 && size<=65536,"invalid save file length");
    Bytes b(static_cast<std::size_t>(size));f.seekg(0);f.read(reinterpret_cast<char*>(b.data()),size);
    valid(bool(f),"save read failed");return b;
}
}
bool map_allows_save(int id) {return id!=11 && id!=13 && id!=15 && id!=82;} // ae.a(int).
std::vector<std::uint8_t> encode_save(const SaveGame& s) {
    validate(s);Bytes body;
    for(int n:{s.character_class,s.map_id,s.zone,s.x,s.y,s.direction,s.quick_item_type}) u32(body,n);
    u32(body,std::uint32_t(s.money));u32(body,std::uint32_t(std::uint64_t(s.money)>>32));
    const auto bank=[&](const Bytes& b){u32(body,b.size());body.insert(body.end(),b.begin(),b.end());};
    bank(s.switches);bank(s.quests);
    const auto item=[&](const InventoryStack& v){u32(body,v.item_type);u32(body,v.item_id);u32(body,v.count);u32(body,v.identified?1:0);
        u32(body,std::uint32_t(v.refine));for(auto b:v.bonus) u32(body,std::uint32_t(std::int32_t(b)));u32(body,std::uint32_t(std::int32_t(v.option)));};
    u32(body,s.inventory.size());for(const auto& v:s.inventory) item(v);
    for(const auto& v:s.equipment) {u32(body,v?1:0);if(v) item(*v);}
    for(int n:{s.vitals.hp,s.vitals.max_hp,s.vitals.sp,s.vitals.max_sp,s.progress.level,
               s.progress.experience,s.progress.attribute_points}) u32(body,n);
    for(int n:s.progress.attributes) u32(body,n);
    u32(body,s.guardians.equipped);
    for(const auto& g:s.guardians.slots) {u32(body,g?1:0);if(g)
        for(int n:{g->id,g->level,g->experience,g->calm,g->wild}) u32(body,n);}
    u32(body,s.progress.max_combo); // version 2
    u32(body,std::uint32_t(s.new_game_plus));u32(body,std::uint32_t(s.clear_progress)); // version 4
    bank(s.recipes); // version 5
    // version 3: items carry refine / four bonuses / option; version 4: New Game+ and banked progress;
    // version 5: the recipe journal.
    Bytes out{'H','L','P','O','R','T',0,0};u32(out,5);u32(out,body.size());
    u32(out,crc32(0,body.data(),static_cast<uInt>(body.size())));
    out.insert(out.end(),body.begin(),body.end());return out;
}
SaveGame decode_save(const Bytes& bytes) {
    valid(bytes.size()>=20 && bytes.size()<=65536,"invalid save length");
    const Bytes magic{'H','L','P','O','R','T',0,0};valid(std::equal(magic.begin(),magic.end(),bytes.begin()),"not a Heroes Lore port save");
    Reader r{bytes,8};const auto version=r.number();valid(version>=1 && version<=5,"unsupported save version");r.version=version;valid(r.number()==bytes.size()-20,"invalid save payload length");
    const auto crc=r.number();valid(crc==crc32(0,bytes.data()+20,static_cast<uInt>(bytes.size()-20)),"save checksum mismatch");
    SaveGame s;s.character_class=r.integer();s.map_id=r.integer();s.zone=r.integer();s.x=r.integer();s.y=r.integer();s.direction=r.integer();s.quick_item_type=r.integer();
    const std::uint64_t low=r.number(),high=r.number();valid(high==0,"saved money out of range");s.money=static_cast<std::int64_t>(low);
    s.switches=r.bank();s.quests=r.bank();const auto count=r.number();valid(count<=30,"saved inventory too large");
    for(unsigned i=0;i<count;++i) s.inventory.push_back(r.item());
    for(auto& v:s.equipment) {const auto present=r.number();valid(present<=1,"invalid equipment presence");if(present) v=r.item();}
    s.vitals.hp=r.integer();s.vitals.max_hp=r.integer();s.vitals.sp=r.integer();s.vitals.max_sp=r.integer();
    s.progress.level=r.integer();s.progress.experience=r.integer();s.progress.attribute_points=r.integer();
    for(auto& n:s.progress.attributes) n=r.integer();
    s.guardians.equipped=r.number();
    for(auto& g:s.guardians.slots) {const auto present=r.number();valid(present<=1,"invalid guardian presence");
        if(present) g=Guardian{r.integer(),r.integer(),r.integer(),r.integer(),r.integer()};}
    if(version>=2) s.progress.max_combo=r.integer();
    if(version>=4) {s.new_game_plus=r.integer();s.clear_progress=r.integer();}
    if(version>=5) {
        const auto size=r.number();valid(size<=8 && r.b.size()-r.p>=size,"invalid saved recipe journal");
        s.recipes.assign(r.b.begin()+std::ptrdiff_t(r.p),r.b.begin()+std::ptrdiff_t(r.p+size));r.p+=size;
    }
    valid(r.p==bytes.size(),"unexpected save trailing data");validate(s);return s;
}
SaveGame read_save(const std::string& path,bool* backup) {
    if(backup) *backup=false;
    try {return decode_save(read_bytes(path));}
    catch(const std::exception& primary) {
        const std::string first=primary.what();
        try {auto s=decode_save(read_bytes(path+".bak"));if(backup) *backup=true;return s;}
        catch(const std::exception& secondary) {throw std::runtime_error("Save: "+first+"; backup: "+secondary.what());}
    }
}
void write_save(const std::string& path,const SaveGame& game) {
    namespace fs=std::filesystem;
    const auto bytes=encode_save(game);const auto temp=path+".tmp";
    auto* f=std::fopen(temp.c_str(),"wb");valid(f!=nullptr,"cannot open temporary save");
    const bool written=std::fwrite(bytes.data(),1,bytes.size(),f)==bytes.size();
    const bool flushed=std::fflush(f)==0;const bool closed=std::fclose(f)==0;
    valid(written && flushed && closed,"save write failed");
    // Validate the complete temporary file before touching the previous slot.
    decode_save(read_bytes(temp));
    std::string previous;
    if(fs::exists(path)) {
        bool good=false;try {decode_save(read_bytes(path));good=true;} catch(const std::exception&) {}
        previous=path+(good?".bak":".corrupt");
        if(fs::exists(previous)) fs::remove(previous);
        fs::rename(path,previous);
    }
    try {fs::rename(temp,path);}
    catch(...) {if(!previous.empty()) {std::error_code ec;fs::rename(previous,path,ec);}throw;}
}

int run_progress_percent(const SaveGame& game) {
    const auto bit=[](const std::vector<std::uint8_t>& bank,int index) {
        return std::size_t(index/8)<bank.size() && (bank[std::size_t(index/8)]>>(index%8)&1);
    };
    int done=0;
    for(int i=0;i<20;++i) done+=bit(game.quests,2+i*3);
    for(int i=100;i<=105;++i) done+=bit(game.switches,i);
    const int divisor=game.character_class==6?19:game.character_class==7?21:game.character_class==8?16:0;
    static constexpr int share[3]{60,30,10};
    if(game.new_game_plus>=3 || game.new_game_plus<0) return 0;
    return divisor?done*share[game.new_game_plus]/divisor:0;
}
int save_progress_percent(const SaveGame& game) {return game.clear_progress+run_progress_percent(game);}
