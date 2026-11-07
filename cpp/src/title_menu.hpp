#pragma once
#include <string>
#include <array>
#include <algorithm>
#include <utility>
#include <vector>
enum class TitlePage { logo, menu, options, help, reading, character, character_confirm,
                       guardian_intro, guardians, guardian_confirm, loading,
                       load_game, overwrite_warning, quit_confirm, splash };
enum class TitleAction { none, continue_game, start_game, quit };
// n.p(): one record per hero that has a save -- hero id 6..8, level, progress %.
struct TitleSaveEntry { int hero{6},level{1},progress{}; bool autosave{}; }; // port: autosave.hls row
// bg.paint state 1: two falling feathers (img/title2, the right one mirrored) and a blinking PRESS ANY KEY.
struct TitleFeather { int x{},y{},phase{}; bool initialised{}; };
struct TitleLogoAnimation {
    std::array<TitleFeather,2> feathers;
    int ticks{};
    unsigned seed{12345u};
    int random(int low,int high) { // h.a(int,int): inclusive range
        seed=seed*1103515245u+12345u;
        return low+int((seed>>16)%unsigned(high-low+1));
    }
    void reset(int index,int width) {
        auto& f=feathers[std::size_t(index)];
        if(index==0) {f.x=random(10,width/2-10);f.y=-10*random(0,4);}
        else {f.x=random(width/2+10,width-10);f.y=-10*random(3,7);}
        f.phase=random(0,7);f.initialised=true;
    }
    // One game step in screen pixels of the original-scale view (width x height).
    void tick(int width,int height) {
        for(int i=0;i<2;++i) {
            auto& f=feathers[std::size_t(i)];
            if(!f.initialised) reset(i,width);
            const int sway=f.phase<4?1:-1;
            f.x+=10*(i==0?sway:-sway);
            f.y+=random(-1,4);
            f.phase=(f.phase+1)%8;
            if(f.y>height+10) reset(i,width);
        }
        ++ticks;
    }
    static int frame(int phase) {return phase<4?phase:8-phase;}
    bool prompt_visible() const {return ticks%4<2;}
};
// bg state 10: img/logo frame 4 on white eases down to the centre, jitters, then flies up with
// a doubling speed; when it has left the screen bg.c() opens the title (state 1).
struct TitleSplash {
    int y{-20}, speed{1}, count{};
    bool done{};
    void tick(int centre, int height) {
        if(count>40) speed*=2;
        if(count==0) {
            if(y<centre-1) y+=(centre-y)/2;
            else count=1;
        } else {
            if(count==1 || count==3) y=centre-1;
            else if(count==2 || count==4) y=centre;
            ++count;
        }
        if(speed>height) done=true;
    }
};
struct TitleMenu {
    bool active{},continue_available{},in_game{};
    int selection{},character_class{6},guardian_focus{},help_topic{},text_page{},game_speed{2};
    bool yes{};
    bool heroes_unlocked{}; // bs.b: Reah/Kail open after the first ending (n.c)
    bool locked_notice{};   // c: af "Unlock this character / after doing Ronin's story"
    bool instant_speech{},smooth_camera{true};
    bool sound{true};        // be row 0: bs.a level 0 (Off) or bw.a (On)
    bool soundfont{true};    // port row 5: music via a .sf2 found next to the game
    bool soundfont_available{};
    bool smooth_motion{true}; // port row 6: draw movement between logic ticks at the display rate
    bool wide_ui{true};      // port row 7: Java (original 155px boxes) or Wide menus
    bool autosave{true};     // port row 8: write autosave.hls on every map entry
    bool rumble{true};       // port row 9: pad rumble on damage and screen shake
    bool modern_ui{};        // port row 7, third value: Modern menus (implies wide_ui)
    int language{};          // port option: 0 JAR text, 1 built-in Russian pack
    std::array<bool,3> chosen_guardians{};
    TitlePage page{TitlePage::logo};
    std::string message;
    std::vector<TitleSaveEntry> saves;
    TitleLogoAnimation logo;
    TitleSplash splash;
    int load_selection{};
    // Port Options tabs: Sound, Display, Interface, Game. On the options page selection 0 is the
    // tab strip and 1.. the tab's rows; option_ids holds the setting each row edits.
    static constexpr int option_tab_count=4;
    static constexpr std::array<std::array<int,3>,option_tab_count> option_ids{{{0,5,-1},{3,6,-1},{4,7,2},{1,8,9}}};
    int options_tab{};
    static int option_rows(int tab) {int n=0;for(int id:option_ids[std::size_t(tab)]) n+=id>=0;return n;}
    int option_id() const {return selection>0 && selection<=option_rows(options_tab)?option_ids[std::size_t(options_tab)][std::size_t(selection-1)]:-1;}
    void switch_options_tab(int delta) {
        options_tab=(options_tab+delta+option_tab_count)%option_tab_count;
        selection=std::min(selection,option_rows(options_tab));
    }
    // bf.c / a.c: the selection brush grows etc[14] -> [16] -> [18], one step per paint, and
    // restarts whenever the selected row (or the page) changes.
    int brush_step{2},brush_key{-1};
    int brush_row_key() const {return int(page)*16+(page==TitlePage::load_game?load_selection:selection);}
    int brush_stage() const {return brush_key==brush_row_key()?brush_step:0;}
    void tick_brush() {
        if(brush_key!=brush_row_key()) {brush_key=brush_row_key();brush_step=0;}
        if(brush_step<2) ++brush_step;
    }
    TitleMenu(bool show=false,bool available=false,std::string notice={})
        :active(show),continue_available(available),message(std::move(notice)) {}
    int guardian_count() const {return int(chosen_guardians[0])+int(chosen_guardians[1])+int(chosen_guardians[2]);}
    // bs.a[] / bs.a(int): the original uses integer 1000 / {8,10,14}.
    unsigned tick_ms() const {return game_speed==0?125:game_speed==1?100:71;}
    void open_menu() {active=true;page=TitlePage::menu;selection=0;text_page=0;message.clear();}
    void navigate(int direction) {
        if(direction<1 || direction>4) return;
        const int delta=(direction==1 || direction==3)?-1:1;
        if(page==TitlePage::logo || page==TitlePage::loading || page==TitlePage::splash) return;
        if(page==TitlePage::character_confirm || page==TitlePage::guardian_confirm) {yes=!yes;return;}
        // c: slot b runs left to right while the hero is 6+(2-b), so Right lowers the class.
        if(page==TitlePage::character) {if(!locked_notice) character_class=6+(character_class-6-delta+3)%3;return;}
        if(page==TitlePage::guardians) {guardian_focus=(guardian_focus+delta+3)%3;return;}
        if(page==TitlePage::reading) {text_page=std::max(0,text_page+delta);return;}
        if(page==TitlePage::options && (direction==3 || direction==4)) {
            const int id=option_id();
            if(selection==0) switch_options_tab(delta);
            else if(id==1) game_speed=(game_speed+delta+3)%3;
            else if(id==2) instant_speech=!instant_speech;
            else if(id==3) smooth_camera=!smooth_camera;
            else if(id==4) language^=1;
            else if(id==5) {if(soundfont_available) soundfont=!soundfont;}
            else if(id==6) smooth_motion=!smooth_motion;
            else if(id==7) { // Java -> Wide -> Modern
                const int style=((modern_ui?2:wide_ui?1:0)+delta+3)%3;wide_ui=style>=1;modern_ui=style==2;
            }
            else if(id==8) autosave=!autosave;
            else if(id==9) rumble=!rumble;
            else if(id==0) sound=direction==4; // be: Left turns the sound off, Right on
            return;
        }
        if(direction!=1 && direction!=2) return;
        if(page==TitlePage::load_game) {
            if(!saves.empty()) load_selection=(load_selection+delta+int(saves.size()))%int(saves.size());
            return;
        }
        if(page==TitlePage::overwrite_warning || page==TitlePage::quit_confirm) return;
        const int count=page==TitlePage::menu?6:page==TitlePage::options?option_rows(options_tab)+1:4;
        selection=(selection+delta+count)%count;
        // bf: without saved data the cursor steps over "Load Game".
        if(page==TitlePage::menu && selection==1 && !continue_available) selection=(selection+delta+count)%count;
        message.clear();
    }
    TitleAction confirm() {
        message.clear();
        switch(page) {
            case TitlePage::logo:open_menu();break;
            case TitlePage::menu:
                // bf: with saved data New Game first warns (text 3929), Load Game opens class a.
                if(selection==0) {
                    if(continue_available) page=TitlePage::overwrite_warning;
                    else start_character_selection();
                }
                else if(selection==1) {
                    if(continue_available && !saves.empty()) {page=TitlePage::load_game;load_selection=0;}
                    else message="No saved game.";
                }
                else if(selection==2) {page=TitlePage::options;selection=0;}
                else if(selection==3) {page=TitlePage::help;selection=0;}
                else if(selection==4) {page=TitlePage::reading;help_topic=4;text_page=0;}
                else {page=TitlePage::quit_confirm;yes=false;} // bh "Are You Sure?" Yes/No
                break;
            case TitlePage::quit_confirm:return TitleAction::quit;
            case TitlePage::overwrite_warning:start_character_selection();break;
            case TitlePage::load_game:if(!saves.empty()) return TitleAction::continue_game;break;
            case TitlePage::character:
                if(locked_notice) locked_notice=false;
                else if(character_class!=6 && !heroes_unlocked) locked_notice=true;
                else {page=TitlePage::character_confirm;yes=false;}
                break;
            case TitlePage::character_confirm:
                page=yes?TitlePage::guardian_intro:TitlePage::character;break;
            case TitlePage::guardian_intro:page=TitlePage::guardians;guardian_focus=0;break;
            case TitlePage::guardians:
                if(!chosen_guardians[guardian_focus] && guardian_count()>=2) {message="Choose exactly two guardians.";break;}
                chosen_guardians[guardian_focus]=!chosen_guardians[guardian_focus];
                if(guardian_count()==2) {page=TitlePage::guardian_confirm;yes=false;}
                break;
            case TitlePage::guardian_confirm: // bk: No clears both choices and returns to picking
                if(yes && guardian_count()==2) {page=TitlePage::loading;return TitleAction::start_game;}
                chosen_guardians.fill(false);page=TitlePage::guardians;break;
            case TitlePage::help:help_topic=selection;text_page=0;page=TitlePage::reading;break;
            case TitlePage::reading:++text_page;break;
            case TitlePage::options:if(selection>0) navigate(4);break;
            default:break;
        }
        return TitleAction::none;
    }
    void start_character_selection() {page=TitlePage::character;character_class=6;chosen_guardians.fill(false);}
    const TitleSaveEntry* selected_save() const {
        return page==TitlePage::load_game && load_selection<int(saves.size())?&saves[load_selection]:nullptr;
    }
    void back() {
        message.clear();
        // d: Help/Options opened from the in-game System tab return to it.
        if(in_game && (page==TitlePage::help || page==TitlePage::options)) {active=false;in_game=false;return;}
        if(page==TitlePage::logo) open_menu();
        else if(page==TitlePage::menu) selection=5;
        else if(page==TitlePage::reading) {page=help_topic==4?TitlePage::menu:TitlePage::help;selection=help_topic==4?4:help_topic;}
        else if(page==TitlePage::character_confirm) page=TitlePage::character;
        else if(page==TitlePage::character && locked_notice) locked_notice=false;
        else if(page==TitlePage::quit_confirm) {page=TitlePage::menu;selection=5;}
        else if(page==TitlePage::guardian_confirm) {chosen_guardians.fill(false);page=TitlePage::guardians;}
        // bk -> a(-1,-1): back to the hero confirmation that opened the selector.
        else if(page==TitlePage::guardians || page==TitlePage::guardian_intro) {
            chosen_guardians.fill(false);page=TitlePage::character_confirm;yes=false;
        }
        else if(page==TitlePage::load_game) {page=TitlePage::menu;selection=1;}
        else {page=TitlePage::menu;selection=0;}
    }
};
