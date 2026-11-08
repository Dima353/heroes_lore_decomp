#pragma once
#include <SDL.h>
#include "bitmap_font.hpp"
#include "hero_vitals.hpp"
#include "quest_journal.hpp"
#include "guardian.hpp"
#include "hero_progress.hpp"
#include "title_menu.hpp"
#include "viewport.hpp"
#include <optional>
#include <limits>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

enum class VisualScreen { none, dialogue, title_menu, game_menu, pause, inventory, controls, shop, choice, service, notice, ending };
struct VisualItem {
    int type{-1}, id{-1}, count{};
    std::string name, description, stats, comparison;
    int power{-1};          // equipment base attack/defense (cb.a item detail)
    bool weapon{}, identified{true};
    int compare{};          // v: 1 better / -1 worse than the equipped piece (shop markers)
    bool stackable{};       // ad.b[type]: quantity can be chosen in ab
    std::uint32_t price{};
    int refine{}, refine_max{}; // e.e / e.d
    std::string option_name;    // t.c attribute (itm/itmatt), drawn in red
    std::string type_name;      // ad.b type name shown for unidentified gear
};
// as.d / as.e frame of the ending (ending.hpp drives it).
struct EndingView {
    int hero{6};
    bool credits{};
    int fade{-1};             // white level of the opening fade, -1 when done
    int first{}, counter{};   // as.t (page start) and as.e (page timer)
    std::vector<std::string> page;
    std::vector<std::pair<std::string,int>> rolling; // staff roll text and y
};
// aa / ax window state resolved to text; the rules live in service.hpp.
struct ServiceView {
    bool blacksmith{true};
    int page{-1}, menu{}, cursor{};
    const std::vector<std::string>* text{};  // sgui/blak or sgui/refi
    bool picking{};
    std::vector<VisualItem> picks;
    int pick_sel{}, pick_title{};
    std::string message;
    bool question{}, busy{}, mix_confirm{};
    VisualItem item, orb;
    bool has_item{}, has_orb{};
    std::array<VisualItem,3> mix;
    std::array<bool,3> has_mix{};
    std::string option_name;
    int cost{}, need{};
};
enum class ButtonStyle : std::uint8_t { keyboard, xbox, nintendo, playstation };
// ae.b(Graphics), as state 11: the map grid at 2px per cell with the hero cell blinking.
struct MinimapView {
    int width{}, height{}, hero_x{}, hero_y{};
    bool hero_lit{};                 // ae.d, flipped once per painted frame
    std::vector<std::uint32_t> cells; // 0xRRGGBB per cell, row-major
};
// Port recipe journal row: the result, its catalogue icon type (-1 unknown) and each part with the count carried.
struct VisualRecipe { std::string result; int icon_type{-1}; std::vector<std::pair<std::string,int>> parts; };
struct VisualUiModel {
    std::optional<MinimapView> minimap;
    std::optional<MinimapView> bottom_minimap; // 3DS: the always-on minimap on the bottom screen
    VisualScreen screen{VisualScreen::none};
    int game_over_chars{-1};   // as.b(Graphics): characters of ce.g[32] revealed, -1 when not shown
    std::string game_over_text;
    const ServiceView* service{};
    const EndingView* ending{};
    std::string title, text, notice, modal_title, modal_text, location_name;
    std::string enemy_name;
    int enemy_level{},enemy_hp{},enemy_max_hp{},enemy_rank{};
    std::vector<VisualItem> items, equipped;
    std::vector<QuestJournalEntry> quests;
    GuardianRoster guardians;
    std::vector<std::string> guardian_text;
    bool quest_reading{};
    TitleMenu startup;
    const BitmapFont* startup_heading_font{};
    std::optional<WorldViewport> world_viewport;
    std::size_t dialogue_visible_end{std::numeric_limits<std::size_t>::max()};
    std::vector<std::string> help_text;
    std::vector<std::string> menu_text; // sgui/gm.tdf (ai.a strings)
    std::vector<std::string> common_text; // sgui/com.tdf (ce.g strings)
    std::string about_text;
    const std::vector<std::string>* language{}; // lang bank for system strings (3903 Progress:, 3929)
    // Port-only English strings use the base JAR font: the Russian font maps Latin codes to Cyrillic glyphs.
    const BitmapFont* latin_font{};
    SDL_Texture* latin_font_texture{};
    bool localized{};
    bool wide_ui{};   // port: widen the 155px menu boxes on wide screens
    bool modern_ui{}; // port: Modern menu layout (tab column, named lists, detail panel)
    bool tab_focus{}; // Modern: the cursor is in the tab column, not the list
    bool recipes_view{}; std::vector<VisualRecipe> recipes; // Quest tab, Y: the recipe journal
    // Platform button names replacing the phone keypad labels baked into the original art.
    std::string key_guardian_calm{"1"}, key_guardian_wild{"3"}, key_item_use{"Q"}, key_item_next{"E"};
    // Hints name the Switch face buttons (A confirm, B back, X, Y); this picks what is drawn for them.
    ButtonStyle buttons{ButtonStyle::keyboard};
    int loading_progress{};
    std::size_t selection{};
    std::int64_t gold{};
    HeroVitals vitals;
    HeroProgress progress;
    std::vector<std::string> hero_text;
    int character_class{6}, attack{}, defense{};
    bool attribute_editing{};
    int attribute_selection{};
    std::array<int,4> pending_attributes{};
    int guardian_choice{-1}, skill_selection{};          // bm Recall/Skill popup, bn skill row
    bool skill_screen{}, skill_key_calm{true};
    int item_action{}, item_choice{};                    // ay af dialog: 4 Equip/Trash, 5 Use/Trash, 6 confirm
    std::string item_prompt;
    bool shop_asking{};                                  // af question over the ab window
    std::string shop_question;
    const std::vector<std::string>* shop_text{};         // sgui/shop.tdf (bp.a strings)
    const std::vector<std::string>* guardian_skill_text{}; // grd/grdsk.tdf (ce.c strings)
    int menu_tab{}, status_page{}, location_step{10};
    int quick_item_type{-1}, quick_item_count{};
    std::array<int,2> guardian_cooldown{}, guardian_cooldown_max{}; // p.c / p.b of the calm and wild skill
    bool guardian_channel{};                                          // p.i == 2: enemy panel moves down 20px
    bool world_hidden{},save_available{};
    bool hud_elsewhere{}; // 3DS: the HUD strip is drawn on the bottom screen, not over the field
    bool bottom_pass{};   // 3DS: this draw is the bottom screen (menus get its backdrop, no field capture)
    bool logo_prompt{true}; // title logo page: PRESS ANY KEY (3DS: off while the menu is open below)
    unsigned bottom_layers{15}; // bottom screen parts: 1 backdrop, 2 minimap/plate, 4 HUD, 8 enemy panel
    int category{}, portrait{}, scroll_lines{}, detail_page{}, quantity{1};
    bool selling{}, confirming{}, choice_yes{true}, event_active{};
    bool item_box{}; // ah state 4: ITEM's "Inventory is full" / "No item" box instead of a speech box
};
using UiTextureMap = std::unordered_map<int, SDL_Texture*>;
// Shared styles and texture keys: uifrm frame IDs, 100+portrait ID, 200+item type, 300+gmico frame.
void draw_world_hud(SDL_Renderer* renderer, const VisualUiModel& model, const UiTextureMap& textures,
    SDL_Texture* font_texture, const BitmapFont& font, int output_w, int output_h, int scale = 0);
void draw_enemy_panel(SDL_Renderer* renderer, const VisualUiModel& model, const UiTextureMap& textures,
    SDL_Texture* font_texture, const BitmapFont& font, int x, int y, int es);
SDL_Rect bottom_button_rect(int index); // 3DS bottom screen: the five touch buttons
bool bottom_minimap_cell(const MinimapView& map, int x, int y, SDL_Rect* out); // one minimap cell there
// Touch targets the last draw_visual_ui call laid out (menu and shop): tabs and list rows,
// each with whether it holds the cursor. modern: the Modern layout (Right leaves the tab column).
struct UiHit { SDL_Rect rect{}; int kind{}; int index{}; bool selected{}; bool modern{}; }; // kind 1 tab, 2 row, 3/4 attribute -/+, 5 confirm, 6 choice-list row, 7 key button (index 0 A, 1 B, 2 X, 3 Y), 9 guardian key tab, 10 quantity -/+
const std::vector<UiHit>& visual_ui_hits();
const std::vector<UiHit>& bottom_screen_hits(); // 3DS field layout: kind 7 key (0 A, 1 B), 11 Yes / No
void draw_bottom_screen(SDL_Renderer* renderer, const VisualUiModel& model, const UiTextureMap& textures,
    SDL_Texture* font_texture, const BitmapFont& font, bool hud, bool menu, const std::function<void(SDL_Rect)>& hero_preview);
void draw_visual_ui(SDL_Renderer* renderer, const VisualUiModel& model,
    const UiTextureMap& textures, SDL_Texture* font_texture, const BitmapFont& font,
    int output_w, int output_h, const std::function<void(SDL_Rect)>& hero_preview);
