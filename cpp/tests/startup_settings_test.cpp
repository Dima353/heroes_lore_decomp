#include "port_settings.hpp"
#include "dialogue_reveal.hpp"
#include "title_menu.hpp"
#include "viewport.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <chrono>
static void check(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
int main(int argc,char** argv) {
    try {
        const std::string path=argc>1?argv[1]:(std::filesystem::temp_directory_path()/
            ("heroes-lore-settings-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))).string();
        check(!std::filesystem::exists(path),"fixture path must be fresh");
        auto defaults=read_port_settings(path);
        check(defaults.game_speed==2 && !defaults.instant_speech && defaults.smooth_camera,"original defaults");
        write_port_settings(path,{0,true,false});auto loaded=read_port_settings(path);
        check(loaded.game_speed==0 && loaded.instant_speech && !loaded.smooth_camera,"settings survive restart");
        check(loaded.smooth_motion,"60 FPS defaults to on");
        {PortSettings off{};off.smooth_motion=false;off.soundfont=false;write_port_settings(path,off);}
        loaded=read_port_settings(path);check(!loaded.smooth_motion && !loaded.soundfont && loaded.sound,"60 FPS flag survives restart");
        {PortSettings java{};java.wide_ui=false;write_port_settings(path,java);}
        check(!read_port_settings(path).wide_ui && read_port_settings(path).smooth_motion,"Java interface survives restart");
        {PortSettings off{};off.autosave=false;write_port_settings(path,off);}
        check(!read_port_settings(path).autosave && read_port_settings(path).wide_ui,"Autosave off survives restart");
        {PortSettings modern{};modern.modern_ui=true;write_port_settings(path,modern);}
        check(read_port_settings(path).modern_ui && read_port_settings(path).autosave,"Modern menus survive restart");
        {PortSettings quiet{};quiet.rumble=false;quiet.modern_ui=true;write_port_settings(path,quiet);}
        check(!read_port_settings(path).rumble && read_port_settings(path).modern_ui,"Rumble off survives restart (byte 10 bit 7)");
        write_port_settings(path,{0,true,false});
        write_port_settings(path,{1,false,true});
        {std::ofstream damaged(path,std::ios::binary|std::ios::trunc);damaged<<"bad";}
        loaded=read_port_settings(path);check(loaded.game_speed==0 && loaded.instant_speech,"corrupt primary uses backup");
        write_port_settings(path,{2,false,true});
        check(read_port_settings_file(path+".bak")->game_speed==0,"saving after corruption preserves good backup");
        {std::ofstream damaged(path,std::ios::binary|std::ios::trunc);damaged<<"bad";}
        std::filesystem::remove(path+".bak");
        check(read_port_settings(path).game_speed==2,"corrupt settings fall back to defaults");
        std::filesystem::remove(path);
        std::filesystem::remove(path+".corrupt");
        TitleMenu menu;menu.page=TitlePage::options;menu.selection=0;
        menu.navigate(4);check(menu.options_tab==1 && menu.selection==0,"Right on the tab strip turns the tab");
        menu.navigate(2);menu.confirm();check(!menu.smooth_camera,"Display tab: Camera option changes behavior flag");
        menu.navigate(2);menu.navigate(4);check(!menu.smooth_motion,"Display tab: 60 FPS option toggles");
        menu.navigate(2);check(menu.selection==0,"rows wrap back to the tab strip");
        menu.switch_options_tab(1);menu.navigate(1);menu.navigate(4);check(menu.instant_speech,"Interface tab: Speech is the last row");
        menu.navigate(1);menu.navigate(4);check(menu.wide_ui && menu.modern_ui,"Interface tab: Menus Wide -> Modern");
        menu.navigate(4);check(!menu.wide_ui && !menu.modern_ui,"Interface tab: Menus Modern -> Java");
        menu.switch_options_tab(1);check(menu.selection==2,"tab change keeps the row when it exists");
        menu.navigate(4);check(!menu.autosave,"Game tab: Autosave option toggles");
        menu.navigate(2);menu.navigate(4);check(!menu.rumble,"Game tab: Rumble option toggles");menu.navigate(1);
        menu.switch_options_tab(-3);check(menu.options_tab==0 && menu.selection==2,"tabs wrap");
        menu.navigate(1);menu.navigate(3);check(!menu.sound,"Sound tab: Left turns the sound off");
        DialogueReveal reveal;const std::string text="A\xc3\xa9 B.\nSecond page";
        reveal.tick(text,0,6,false);check(reveal.end==3,"two characters, UTF-8 boundary preserved");
        reveal.tick(text,0,6,false);check(reveal.end==5,"spaces consume character steps");
        reveal.tick(text,0,6,false);check(reveal.complete(6),"reveal stops at page end");
        reveal.reset();reveal.tick(text,6,text.size(),true);check(reveal.complete(text.size()),"instant page");
        reveal.reset();reveal.tick(text,6,text.size(),false);check(reveal.end==8,"next page starts at its own offset");
        reveal.end=text.size();reveal.tick(text,0,text.size(),false);check(reveal.end==text.size(),"acknowledged page stays complete");
        WorldCamera camera;WorldViewport target{3,-300,-90,48};
        auto view=camera.update(target,4,true,true);check(view.left==-300,"initial camera snaps");
        target.left=-360;target.top=-150;view=camera.update(target,4,true,false);
        check(view.left==-300,"camera advances on game ticks only");
        view=camera.update(target,4,true,true);check(view.left<-300 && view.left>-360 && view.top==-90,"smooth facing axis");
        view=camera.update(target,1,false,true);check(view.left==-360 && view.top==-150,"Camera Off follows directly");
        camera.reset();target.left=-900;view=camera.update(target,4,true,true);
        check(view.left==-900,"map or load reset does not pan across worlds");
        std::cout<<"Startup settings/reveal/camera checks passed\n";
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
