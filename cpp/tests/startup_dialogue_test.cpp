#include "event_runtime.hpp"
#include "language.hpp"
#include "viewport.hpp"
#include "event_visual.hpp"
#include <iostream>
#include <stdexcept>
static void check(bool ok,const char* text) {if(!ok) throw std::runtime_error(text);}
int main() {
    try {
        check(format_dialogue_text("|[Ronin] Why?")=="[Ronin] Why?","pipe speaker marker hidden");
        check(format_dialogue_text("$[Refiner] What?")=="[Refiner] What?","speaker color marker hidden");
        check(format_dialogue_text("Why?;[Unknown]")=="Why?\n[Unknown]","ordinary punctuation preserved");
        GameMap map;EventGrid grid;EventRuntime event(map,grid,{1});
        const std::vector<std::uint8_t> script{1,0,0,4,3,0,9,5,0,2,0,3,10,0,25,1,1,0,99,0,0};
        check(event.step(script).status==EventStepStatus::wait,"text waits for Next");
        event.request_skip();
        check(event.step(script).status==EventStepStatus::applied,"Skip advances text");
        // ah.d never reads a:Z: Skip fast-forwards delays tick by tick instead of cutting them.
        check(event.step(script).remaining_ticks==2 && event.step(script).remaining_ticks==1 &&
              event.step(script).status==EventStepStatus::applied && event.cursor()==2,"Skip keeps delay ticks");
        event.step(script);check((event.switch_bits()[0]&32)!=0,"Skip preserves script mutations");
        check(event.step(script).status==EventStepStatus::wait && event.cursor()==3,"Skip does not answer choices");
        check(!event.skipping(),"YES/NO clears Skip");
        event.step(script,true);event.step(script);
        check(event.money()==325,"Skip still applies reward after choice");
        check(event.step(script).status==EventStepStatus::wait && event.cursor()==5,"text after choice is shown");
        EventRuntime unknown(map,grid);unknown.request_skip();
        check(unknown.step({98,0,0}).status==EventStepStatus::delegated,"Skip cannot silently discard unknown commands");
        for(int opcode:{24,25}) {
            EventRuntime effect(map,grid);auto visual=make_event_visual_state(grid);
            const std::vector<std::uint8_t> actions{std::uint8_t(opcode),255,255,10,0,5,99,0,0};
            const int duration=opcode==24?5:3; // h() runs in the dispatch frame, then one closing frame
            for(int remaining=duration;remaining>=0;--remaining) {
                auto frame=effect.step(actions);
                check(frame.status==EventStepStatus::visual_command && frame.remaining_ticks==remaining,"effect has fixed original duration");
                check(!event_step_continues_frame(frame),"effect phase must be presented before next command");
                check(effect.cursor()==std::size_t(remaining==0?1:0),"effect blocks following mutations");
                apply_event_visual_command(visual,frame.command,frame.remaining_ticks);
                if(opcode==24) check(visual.flash_white==(remaining%2==1),"flash alternates white and world");
                else check(visual.shake_x>=-5 && visual.shake_x<=5 && visual.shake_y>=-5 && visual.shake_y<=5,"shake offsets stay in original range");
            }
            check(!visual.flash_white && visual.shake_x==0 && visual.shake_y==0,"effect cleans up final frame");
            effect.step(actions);check(effect.money()==305,"reward applies after effect");
            EventRuntime skipped(map,grid);auto begin=skipped.step(actions);
            begin=skipped.step(actions);apply_event_visual_command(visual,begin.command,begin.remaining_ticks);
            skipped.request_skip();auto end=skipped.step(actions);
            check(skipped.cursor()==0 && end.remaining_ticks==duration-2,"Skip does not shorten effect");
            while(skipped.cursor()==0) end=skipped.step(actions);
            apply_event_visual_command(visual,end.command,end.remaining_ticks);
            check(!visual.flash_white && visual.shake_x==0 && visual.shake_y==0,"effect resets visuals");
            skipped.step(actions);check(skipped.money()==305,"Skip retains following reward");
        }
        const auto dialogue=calculate_dialogue_layout(1280,720,8);
        check(dialogue.panel.y+dialogue.panel.h>720-108 && dialogue.panel.y+dialogue.panel.h<720,
              "dialogue overlays HUD and leaves softkey line");
        std::cout<<"Startup dialogue checks passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
