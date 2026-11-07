#pragma once
#include "equipment.hpp"
#include "inventory_rules.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

// Blacksmith aa (/sgui/blak: at refine, ch identify) and refiner ax (/sgui/refi: ap give
// attribute, k mix). Item references follow m: 100+slot for an equipped piece, else the bag index.
struct ServiceInventory {
    std::vector<InventoryStack>& bag;
    EquipmentSet& equipment;
    std::int64_t& money;
    const std::vector<ShopItem>& catalog;
};
InventoryStack* service_item(const ServiceInventory& inv, int ref);
// ao.a(boolean,byte): equipped pieces first, then g.a(boolean,byte) over the bag. `attributable`
// keeps t (types 0..3); identified 1 = identified only, -1 = unidentified only, 0 = any.
std::vector<int> equipment_choices(const ServiceInventory& inv, bool attributable, int identified);
std::vector<int> bag_choices_of_type(const std::vector<InventoryStack>& bag, int type); // g.a(byte)
std::vector<int> mixable_choices(const std::vector<InventoryStack>& bag);               // g.b(): ad.c
bool is_mixable_item(int type);
int count_items(const std::vector<InventoryStack>& bag, int type, int id);               // g.a(byte,byte)
// Removes `count` items from the first matching stacks; `tracked` follows a bag index across erasures.
void remove_items(std::vector<InventoryStack>& bag, int type, int id, int count, int* tracked = nullptr);
// g.a(ad,int): stacks onto a matching stack with room, else takes a free slot. Returns the bag index or -1.
int add_item(std::vector<InventoryStack>& bag, const InventoryStack& item);

// itm/mixtbl: n, n (type,id) parts, then the (type,id) result; ad.a(ad,ad,ad) needs every part
// matched by a distinct chosen item and exactly n chosen items.
struct MixRecipe { std::vector<std::pair<int,int>> parts; std::pair<int,int> result; };
std::vector<MixRecipe> parse_mix_table(const std::vector<std::uint8_t>& bytes);
std::optional<std::pair<int,int>> find_mix(const std::vector<MixRecipe>& table, const std::vector<std::pair<int,int>>& chosen);

enum class ServiceKind { blacksmith, refiner };
// Message entries: >= 0 index into the service TDF, < 0 language bank index -n.
struct ServiceSession {
    ServiceKind kind{};
    int page{-1};       // -1: af type 8 menu; 0 at / ap, 1 ch / k
    int menu{};         // af list cursor
    int cursor{};       // b: focused widget of the page
    bool picking{}, result_view{};
    std::vector<int> picks;
    int pick_sel{}, pick_title{};
    std::vector<int> message;
    bool question{}, mix_confirm{};
    int item{-1}, orb{-1};
    std::array<int,3> mix{{-1,-1,-1}};
    int cost{}, need{};          // at: e*100 gold, e+1 Mithril
    int busy{};                  // at.d: 2 "Under refining." popup pending resolution
    bool success{}, autosave{}, done{};

    int widgets() const { return kind == ServiceKind::blacksmith ? 2 : page == 0 ? 3 : 4; }
    void navigate(int direction);
    void back();
    void confirm(const ServiceInventory& inv, const std::vector<MixRecipe>& mix_table, const std::function<int(int,int)>& roll);
    // Called when the refining pause ends (500 ms, plus 1000 ms on success).
    void finish_refine(const ServiceInventory& inv);
private:
    void open_picker(std::vector<int> choices, int title, int empty_message);
    void pick(const ServiceInventory& inv, int ref);
    void press(const ServiceInventory& inv);
    void apply(const ServiceInventory& inv, const std::vector<MixRecipe>& mix_table, const std::function<int(int,int)>& roll);
    void set_refine_item(const ServiceInventory& inv, int ref);
};
