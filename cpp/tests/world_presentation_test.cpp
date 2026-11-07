#include "world_presentation.hpp"
#include "language.hpp"
#include "quest_journal.hpp"
#include <iostream>
#include <stdexcept>
static void check(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
int main(int argc,char** argv) {
    try {
        MapEntity object;object.y=3;object.offset_y=-4;
        MapEntity npc;npc.y=2;npc.partial_y=true;
        auto behind=world_draw_order({object},{npc},{},32);
        check(behind[0].kind==WorldDrawKind::hero && behind[1].kind==WorldDrawKind::object &&
              behind[2].kind==WorldDrawKind::npc,"hero behind object and moving NPC");
        auto front=world_draw_order({object},{npc},{},64);
        check(front.back().kind==WorldDrawKind::hero,"hero below object draws in front");
        const std::vector<std::string> bank{"Town","Leslay's House"};
        check(parse_tdf_text({2,0,1,'0',0,3,' ','1',' '},bank)==bank,"TDF decimal references and trimming");
        for(const auto& bad: {std::vector<std::uint8_t>{},std::vector<std::uint8_t>{1,0,2,'0'},
                            std::vector<std::uint8_t>{1,0,1,'9'}}) {
            bool rejected=false;try { parse_tdf_text(bad,bank); } catch(const std::runtime_error&) { rejected=true; }
            check(rejected,"invalid TDF rejected");
        }
        if(argc>1) {
            GameArchive archive(argv[1]);
            const auto language=load_language_bank(archive,"lang/language.fr-FR");
            const auto names=parse_tdf_text(archive.read_entry("m/name.tdf"),language);
            check(names.size()==86 && !names[66].empty(),"real location names and Leslay house");
            const auto qt=parse_tdf_text(archive.read_entry("sgui/q6.tdf"),language);
            std::vector<std::uint8_t> bits(128,0);
            check(build_quest_journal(6,qt,bits).empty(),"unstarted quests hidden");
            bits[19/8]|=1u<<(19%8);
            auto quests=build_quest_journal(6,qt,bits);
            check(quests.size()==1 && quests[0].id==6 && !quests[0].main_quest && quests[0].name==qt[42],
                  "Reference book 1 quest uses real start bit and title");
            bits[21/8]|=1u<<(21%8);
            quests=build_quest_journal(6,qt,bits);
            check(quests.size()==1 && quests[0].name==qt[44],"second quest phase title");
            bits[20/8]|=1u<<(20%8);
            check(build_quest_journal(6,qt,bits).empty(),"completed quest hidden");
            std::cout<<names[0]<<" / "<<names[66]<<'\n';
        }
        std::cout<<"World presentation checks passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
