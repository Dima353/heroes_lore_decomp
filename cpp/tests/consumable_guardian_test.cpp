#include "consumable.hpp"
#include "event_runtime.hpp"
#include <iostream>
#include <stdexcept>
static void check(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
int main() {
    try {
        std::vector<InventoryStack> bag{{7,0,2},{9,0,1},{10,0,1}};
        HeroVitals vitals{10,72,5,60};
        check(use_consumable(bag,0,vitals)==ItemUseResult::used && vitals.hp==24 && bag[0].count==1,
              "HP uses 20 percent with integer rounding and one item");
        check(use_consumable(bag,1,vitals)==ItemUseResult::used && vitals.sp==23 && bag.size()==2,
              "SP uses 30 percent and removes exhausted stack");
        {
            const int before=bag[1].count;bool antidote=false;
            check(use_consumable(bag,1,vitals,&antidote)==ItemUseResult::used && antidote &&
                  (bag.size()<2 || bag[1].item_type!=10 || bag[1].count==before-1),"antidote is spent and cures");
        }
        check(use_consumable(bag,99,vitals)==ItemUseResult::invalid_slot,"invalid slot is atomic");
        vitals.hp=72;
        check(use_consumable(bag,0,vitals)==ItemUseResult::used && vitals.hp==72 && bag.empty(),
              "original still consumes at full HP");
        bag={{8,0,1}};vitals.hp=1;
        check(use_consumable(bag,0,vitals)==ItemUseResult::used && vitals.hp==29,"strong potion restores 40 percent");
        auto guardians=make_guardian_roster({true,true,false});
        check(guardians.slots[0]->id==0 && guardians.slots[1]->id==1 && !guardians.slots[2] &&
              guardians.slots[0]->next_experience()==80 && guardians.slots[0]->wild==-1,
              "initial chosen guardians have correct level and locked wild slot");
        check(!award_guardian(guardians,0),"duplicate guardian not added");
        GameMap map;EventGrid event;
        EventRuntime runtime(map,event,{1},{},{},300,vitals,guardians);
        const std::vector<std::uint8_t> actions{18,0,0,18,1,0,18,2,0,99,0,0};
        for(int i=0;i<3;++i) check(runtime.step(actions).status==EventStepStatus::applied,"GUARDIAN command handled");
        const auto& awarded=runtime.guardians();
        check(awarded.slots[2]->id==4 && awarded.slots[2]->level==10 && awarded.slots[2]->wild==1 &&
              awarded.slots[3]->id==3 && awarded.slots[3]->level==1 &&
              awarded.slots[4]->id==5 && awarded.slots[4]->level==20,"event choices map 0/1/2 to Nord/Rocky/Geb");
        EventRuntime next(map,event,runtime.switch_bits(),runtime.quest_bits(),runtime.inventory(),
            runtime.money(),runtime.vitals(),runtime.guardians());
        check(next.guardians().slots[4]->id==5,"guardian state survives event handoff");
        check(!award_guardian(guardians,1),"switching does not create duplicates");
        std::cout<<"Consumable and guardian checks passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
