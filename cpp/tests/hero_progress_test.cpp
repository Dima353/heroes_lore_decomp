#include "event_runtime.hpp"
#include <iostream>
#include <stdexcept>
static void check(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
int main() {
    try {
        GameMap map;EventGrid event;
        HeroVitals vitals{1,72,1,60};
        const auto guardians=make_guardian_roster({true,true,false});
        EventRuntime runtime(map,event,{1},{},{},300,vitals,guardians);
        const std::vector<std::uint8_t> actions{12,0,20,99,0,0};
        check(runtime.step(actions).status==EventStepStatus::applied,"EXP does not delegate");
        const auto& hero=runtime.progress();
        check(hero.level==2 && hero.experience==0 && hero.attribute_points==3 && hero.next_experience()==164,
              "EXP multiplies by four and levels with remainder");
        check(runtime.vitals().hp==84 && runtime.vitals().sp==72,"level up recalculates and fully heals");
        check(runtime.guardians().slots[0]->level==2 && runtime.guardians().slots[1]->level==1,
              "only active guardian receives experience");
        EventRuntime next(map,event,runtime.switch_bits(),runtime.quest_bits(),runtime.inventory(),
            runtime.money(),runtime.vitals(),runtime.guardians(),runtime.progress());
        check(next.progress().attribute_points==3 && next.progress().level==2,"progress survives handoff");
        EventRuntime packed(map,event,{1},{},{},300,vitals,guardians);
        packed.step({12,1,0});
        check(packed.progress().level==5 && packed.progress().experience==154 &&
              packed.progress().attribute_points==12,"unsigned16 award supports multiple levels");
        auto allocated=hero;auto current=runtime.vitals();
        check(!apply_attribute_points(allocated,current,{0,4,0,0}) && allocated.attribute_points==3,
              "overspending is atomic");
        check(!apply_attribute_points(allocated,current,{-1,0,0,0}),"negative allocation rejected");
        check(apply_attribute_points(allocated,current,{1,1,0,1}) && allocated.attribute_points==0 &&
              allocated.attributes[0]==9 && current.max_hp==96 && current.max_sp==84 &&
              current.hp==84 && current.sp==72,"allocation raises maxima without free healing");
        HeroProgress capped;capped.level=99;HeroVitals capped_vitals;auto roster=guardians;
        add_event_experience(capped,capped_vitals,roster,65535);
        check(capped.level==99 && capped.attribute_points==0,"hero level cap does not grant extra points");
        std::cout<<"Hero progression checks passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
