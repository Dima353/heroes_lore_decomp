#include "title_menu.hpp"
#include <iostream>
#include <stdexcept>
static void check(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
int main() {
    try {
        TitleMenu menu(true,false);
        check(menu.game_speed==2 && menu.tick_ms()==71,"original defaults to 14 ticks per second");
        menu.game_speed=0;check(menu.tick_ms()==125,"original slow speed is 8 ticks per second");
        menu.game_speed=1;check(menu.tick_ms()==100,"original normal speed is 10 ticks per second");
        menu.game_speed=2;
        check(menu.page==TitlePage::logo,"starts with separate logo screen");
        menu.confirm();check(menu.page==TitlePage::menu,"logo opens main menu");
        menu.selection=1;check(menu.confirm()==TitleAction::none && !menu.message.empty(),"missing save does not start game");
        menu.selection=0;menu.navigate(2);check(menu.selection==2,"cursor skips Load Game without saves");
        menu.continue_available=true;menu.saves={{6,10,30},{8,2,4}};menu.selection=1;
        check(menu.confirm()==TitleAction::none && menu.page==TitlePage::load_game,"Load Game opens the save list");
        menu.navigate(2);check(menu.selected_save() && menu.selected_save()->hero==8,"list selects next hero");
        menu.navigate(2);check(menu.load_selection==0,"list wraps");
        check(menu.confirm()==TitleAction::continue_game,"Load game routes correctly");
        menu.back();check(menu.page==TitlePage::menu && menu.selection==1,"Back returns to Load Game item");
        menu.selection=0;menu.confirm();check(menu.page==TitlePage::overwrite_warning,"New Game warns about saved data");
        menu.confirm();check(menu.page==TitlePage::character,"warning continues to hero selection");
        menu.continue_available=false;menu.saves.clear();
        menu.open_menu();menu.confirm();menu.navigate(3);
        check(menu.character_class==7,"Left moves to the slot left of Ronin");
        menu.confirm();check(menu.locked_notice && menu.page==TitlePage::character,"locked hero shows the unlock notice");
        menu.confirm();check(!menu.locked_notice,"Ok closes the notice");
        menu.selection=5;menu.page=TitlePage::menu;menu.confirm();
        check(menu.page==TitlePage::quit_confirm,"Exit asks Are You Sure");
        menu.back();check(menu.page==TitlePage::menu && menu.selection==5,"No returns to Exit");
        menu.confirm();check(menu.confirm()==TitleAction::quit,"Yes quits");
        menu.heroes_unlocked=true;
        for(int character=6;character<=8;++character) {
            menu.open_menu();menu.confirm();
            while(menu.character_class!=character) menu.navigate(4);
            menu.confirm();check(menu.page==TitlePage::character_confirm && !menu.yes,"hero requires confirmation");
            menu.navigate(0);check(!menu.yes,"non-direction key cannot select Yes");
            menu.confirm();check(menu.page==TitlePage::character,"No returns to hero selection");
            menu.confirm();menu.navigate(3);menu.confirm();
            check(menu.page==TitlePage::guardian_intro,"hero Yes opens guardian introduction");
            menu.confirm();menu.confirm();menu.navigate(4);menu.confirm();
            check(menu.guardian_count()==2 && menu.page==TitlePage::guardian_confirm && !menu.yes,"exactly two guardians need confirmation");
            menu.confirm();
            check(menu.page==TitlePage::guardians && menu.guardian_count()==0,"bk: No clears the pair");
            menu.guardian_focus=0;menu.confirm();menu.guardian_focus=2;menu.confirm();
            check(menu.page==TitlePage::guardian_confirm && menu.chosen_guardians[0] && menu.chosen_guardians[2],"new pair chosen");
            menu.back();check(menu.page==TitlePage::guardians && menu.guardian_count()==0,"back in confirmation clears too");
            menu.back();check(menu.page==TitlePage::character_confirm,"back returns to hero confirmation");
            menu.navigate(3);menu.confirm();menu.confirm();
            menu.guardian_focus=0;menu.confirm();menu.guardian_focus=2;menu.confirm();
            menu.navigate(3);check(menu.confirm()==TitleAction::start_game && menu.page==TitlePage::loading,"confirmed setup starts loading");
        }
        menu.open_menu();menu.selection=2;menu.confirm();menu.options_tab=3;menu.selection=1;const auto tick=menu.tick_ms();menu.navigate(4);
        check(menu.tick_ms()!=tick,"game speed changes tick interval");
        menu.back();menu.selection=3;menu.confirm();menu.selection=1;menu.confirm();
        check(menu.page==TitlePage::reading && menu.help_topic==1,"Guardian help page opens");
        menu.back();check(menu.page==TitlePage::help,"Help reader returns to topics");
        std::cout<<"Startup flow checks passed\n";
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
