#include "event_runtime.hpp"
#include "inventory_rules.hpp"
#include <iostream>
#include <stdexcept>
static void check(bool ok,const char* text) {if(!ok) throw std::runtime_error(text);}
int main() {
    try {
        GameMap map;EventGrid grid;
        // ah.q/ah.g: the "inventory full" / "no item" message ends the whole event.
        std::vector<InventoryStack> full(inventory_slot_limit,InventoryStack{0,1,1});
        const std::vector<std::uint8_t> give{11,0,2,21,1,0,10,0,25,99,0,0};
        EventRuntime bag(map,grid,{},{},full);
        auto result=bag.step(give);
        check(result.status==EventStepStatus::wait && bag.item_message_string_index()==3938,"full bag shows message");
        result=bag.step(give,std::nullopt,true);
        check(result.status==EventStepStatus::ended && result.command.opcode==11 && bag.ended(),"message ends event");
        check(bag.step(give).status==EventStepStatus::ended && bag.money()==300,"later reward is not applied");
        const std::vector<std::uint8_t> take{11,20,3,21,255,0,9,5,0,99,0,0};
        EventRuntime missing(map,grid);
        check(missing.step(take).status==EventStepStatus::wait && missing.item_message_string_index()==3939,"missing item message");
        missing.request_skip();
        check(missing.step(take).status==EventStepStatus::ended && (missing.switch_bits()[0]&32)==0,"Skip closes message and ends event");
        // ah.s: MAP_HERO without MAP_CHNG is an in-map teleport.
        EventRuntime teleport(map,grid);
        result=teleport.step({8,10,12,99,0,0});
        check(result.status==EventStepStatus::applied && result.command.arg1==10 && result.command.arg2==12 &&
              teleport.cursor()==1 && event_step_continues_frame(result),"lone MAP_HERO teleports");
        // ah.d: MV_HERO, MV_DELAY 2, STOP gives the hero exactly two moving frames (16 px).
        {
            const std::vector<std::uint8_t> walk{3,2,0,4,2,0,5,0,0,99,0,0};
            EventRuntime script(map,grid);
            int moving_frames=0;bool moving=false;
            for(int frame=0;frame<8 && !script.ended();++frame) {
                for(int steps=0;steps<8;++steps) {
                    const auto r=script.step(walk,std::nullopt,false,steps==0?1:0);
                    if(r.status==EventStepStatus::actor_command) moving=r.command.opcode==3;
                    if(!event_step_continues_frame(r,walk)) break;
                }
                if(moving) ++moving_frames;
            }
            check(moving_frames==2,"scripted walk + delay 2 moves the hero two frames");
        }
        std::cout<<"Event ah checks passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
