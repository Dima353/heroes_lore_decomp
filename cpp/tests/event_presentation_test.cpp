#include "event_visual.hpp"
#include <iostream>
#include <stdexcept>
static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    try {
        GameMap map; map.width=map.height=2; map.terrain.assign(4,0);
        EventGrid event; event.overlay.assign(4,0); event.npcs={0,0,18,1,1,19};
        apply_setup_group(map,event,{111,2,0,0,112,1,9,0});
        auto visual=make_event_visual_state(event);
        check(visual.hero_emote==2 && visual.npc_emotes.size()==2 &&
              visual.npc_emotes[0]==0 && visual.npc_emotes[1]==9,"setup emote survives map load");
        apply_event_visual_command(visual,{50,1,10});
        check(visual.npc_emotes[1]==10,"event can replace initial quest marker");
        apply_setup_group(map,event,{111,0,0,0,112,1,0,0});
        visual=make_event_visual_state(event);
        check(visual.hero_emote==0 && visual.npc_emotes[1]==0,"zero clears setup effects");
        bool rejected=false;
        try { apply_setup_group(map,event,{112,2,9,0}); }
        catch(const std::out_of_range&) { rejected=true; }
        check(rejected,"invalid marker actor rejected");

        EventRuntime healing(map,event,{1},{},{},300,HeroVitals{12,72,7,60});
        const std::vector<std::uint8_t> heal{13,0,20,14,0,10,13,255,255,14,255,255,99,0,0};
        check(healing.step(heal).status==EventStepStatus::applied && healing.vitals().hp==32,
              "HP event adds unsigned packed amount");
        healing.step(heal);
        check(healing.vitals().sp==17,"SP event modifies SP only");
        healing.step(heal);healing.step(heal);
        check(healing.vitals().hp==72 && healing.vitals().sp==60,"HP/SP clamp at maxima");
        EventRuntime transferred(map,event,{1},{},{},300,healing.vitals());
        check(transferred.vitals().hp==72 && transferred.vitals().sp==60,"vitals handoff across events");

        EventRuntime runtime(map,event,{1});
        const std::vector<std::uint8_t> actions{1,0,1,9,3,0,50,1,10,1,1,255,4,3,0,99,0,0};
        auto result=runtime.step(actions);
        check(result.status==EventStepStatus::wait && !event_step_continues_frame(result),"first text waits");
        unsigned steps=0; bool acknowledge=true;
        do {
            result=runtime.step(actions,std::nullopt,acknowledge,0);
            acknowledge=false;
            if(result.status==EventStepStatus::visual_command) apply_event_visual_command(visual,result.command);
            check(++steps<64,"bounded text continuation");
        } while(event_step_continues_frame(result));
        check(result.status==EventStepStatus::wait && result.command.opcode==1 &&
              result.command.arg1==1 && runtime.cursor()==3 && visual.npc_emotes[1]==10,
              "acknowledgement reaches next text in same frame through synchronous commands");
        result=runtime.step(actions,std::nullopt,true);
        check(event_step_continues_frame(result),"text acknowledgement continues");
        result=runtime.step(actions,std::nullopt,false,0);
        check(result.status==EventStepStatus::wait && result.remaining_ticks==2 &&
              !event_step_continues_frame(result),"delay still waits for simulation ticks");
        result=runtime.step(actions,std::nullopt,false,1);
        check(result.remaining_ticks==1,"delay loses only one tick");
        result=runtime.step(actions,std::nullopt,false,1);
        check(result.status==EventStepStatus::applied && !event_step_continues_frame(result),"completed delay consumes tick");
        if(argc>1) {
            GameArchive archive(argv[1]);
            for(const auto& flags: {std::vector<std::uint8_t>{87},std::vector<std::uint8_t>{9},std::vector<std::uint8_t>{10}}) {
                auto leslay_map=load_map(archive,79);
                auto leslay=load_event_grid(archive,"6",79,leslay_map);
                std::vector<std::uint8_t> bits(128,0);bits[0]=1;
                for(auto flag:flags) bits[flag/8]|=std::uint8_t(1u<<(flag%8));
                const auto setup=select_setup_group(leslay,bits);
                check(bool(setup),"Store setup selected");
                apply_setup_group(leslay_map,leslay,setup->second);
                const auto state=make_event_visual_state(leslay);
                check(state.npc_emotes.at(3)==(flags[0]==87?9:flags[0]==9?10:0),
                      "Store before/after/finished quest marker");
            }
            unsigned markers=0;
            for(int zone=6;zone<=8;++zone) for(int id=0;id<=79;++id) {
                GameMap real_map;
                EventGrid real_event;
                try { real_map=load_map(archive,id);real_event=load_event_grid(archive,std::to_string(zone),id,real_map); }
                catch(const std::exception&) { continue; }
                for(const auto& group:real_event.setup_groups) {
                    if(group.size()%4) throw std::runtime_error("unaligned real setup");
                    for(std::size_t i=0;i<group.size();i+=4) if(group[i]==111 || group[i]==112) {
                        auto copy=real_event;
                        apply_setup_group(real_map,copy,{group[i],group[i+1],group[i+2],group[i+3]});
                        auto state=make_event_visual_state(copy);
                        if(group[i]==112) check(state.npc_emotes.at(group[i+1])==group[i+2],"JAR NPC marker");
                        else check(state.hero_emote==group[i+1],"JAR hero emote");
                        ++markers;
                    }
                }
            }
            check(markers>0,"JAR setup markers found");
            std::cout << markers << " JAR setup emotes checked\n";
        }
        std::cout << "Event presentation checks passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
