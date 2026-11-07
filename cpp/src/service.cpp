#include "service.hpp"
#include <algorithm>

namespace {
bool is_equipment(int type) { return type >= 0 && type <= 6; }
bool matches(const InventoryStack& s, bool attributable, int identified) {
    if (!is_equipment(s.item_type) || (attributable && s.item_type > 3)) return false;
    return identified == 0 || (identified == 1) == s.identified;
}
int limit_of(const ServiceInventory& inv, const InventoryStack& s) {
    const auto* item = find_catalog_item(inv.catalog, s.item_type, s.item_id);
    const auto info = item ? equipment_info(*item) : std::nullopt;
    return info ? info->secondary : 0;
}
}

InventoryStack* service_item(const ServiceInventory& inv, int ref) {
    if (ref >= 100) {
        const auto slot = std::size_t(ref - 100);
        return slot < inv.equipment.size() && inv.equipment[slot] ? &*inv.equipment[slot] : nullptr;
    }
    return ref >= 0 && std::size_t(ref) < inv.bag.size() ? &inv.bag[std::size_t(ref)] : nullptr;
}
std::vector<int> equipment_choices(const ServiceInventory& inv, bool attributable, int identified) {
    std::vector<int> out;
    for (std::size_t slot = 0; slot < inv.equipment.size(); ++slot)
        if (inv.equipment[slot] && matches(*inv.equipment[slot], attributable, identified)) out.push_back(100 + int(slot));
    for (std::size_t i = 0; i < inv.bag.size(); ++i)
        if (matches(inv.bag[i], attributable, identified)) out.push_back(int(i));
    return out;
}
std::vector<int> bag_choices_of_type(const std::vector<InventoryStack>& bag, int type) {
    std::vector<int> out;
    for (std::size_t i = 0; i < bag.size(); ++i) if (bag[i].item_type == type) out.push_back(int(i));
    return out;
}
bool is_mixable_item(int type) {
    static constexpr std::array<bool,24> mixable{{false,false,false,false,false,false,false,true,
        false,true,false,false,true,true,true,true,true,false,false,false,false,false,false,false}};
    return type >= 0 && type < 24 && mixable[std::size_t(type)];
}
std::vector<int> mixable_choices(const std::vector<InventoryStack>& bag) {
    std::vector<int> out;
    for (std::size_t i = 0; i < bag.size(); ++i) if (is_mixable_item(bag[i].item_type)) out.push_back(int(i));
    return out;
}
int count_items(const std::vector<InventoryStack>& bag, int type, int id) {
    int total = 0;
    for (const auto& s : bag) if (s.item_type == type && s.item_id == id) total += s.count;
    return total;
}
void remove_items(std::vector<InventoryStack>& bag, int type, int id, int count, int* tracked) {
    for (std::size_t i = 0; i < bag.size() && count > 0;) {
        auto& s = bag[i];
        if (s.item_type != type || s.item_id != id) { ++i; continue; }
        const int taken = std::min(count, s.count);
        s.count -= taken; count -= taken;
        if (s.count > 0) { ++i; continue; }
        bag.erase(bag.begin() + std::ptrdiff_t(i));
        if (tracked && *tracked >= 0 && *tracked < 100) {
            if (*tracked == int(i)) *tracked = -1;
            else if (*tracked > int(i)) --*tracked;
        }
    }
}
int add_item(std::vector<InventoryStack>& bag, const InventoryStack& item) {
    if (is_stackable_item(std::uint8_t(item.item_type)))
        for (std::size_t i = 0; i < bag.size(); ++i)
            if (bag[i].item_type == item.item_type && bag[i].item_id == item.item_id &&
                bag[i].count + item.count <= inventory_stack_limit) {
                bag[i].count += item.count; consolidate_stacks(bag);
                // Pouring can empty earlier stacks and shift i: report the stack that still has room.
                int found = -1;
                for (std::size_t k = 0; k < bag.size(); ++k)
                    if (bag[k].item_type == item.item_type && bag[k].item_id == item.item_id) {
                        found = int(k);
                        if (bag[k].count < inventory_stack_limit) break;
                    }
                return found;
            }
    if (bag.size() >= inventory_slot_limit) return -1;
    bag.push_back(item);
    return int(bag.size() - 1);
}
std::vector<MixRecipe> parse_mix_table(const std::vector<std::uint8_t>& bytes) {
    std::vector<MixRecipe> table;
    for (std::size_t p = 0; p < bytes.size();) {
        MixRecipe recipe;
        const std::size_t n = bytes[p++];
        if (p + n * 2 + 2 > bytes.size()) break;
        for (std::size_t i = 0; i < n; ++i, p += 2) recipe.parts.emplace_back(bytes[p], bytes[p + 1]);
        recipe.result = {bytes[p], bytes[p + 1]};
        p += 2;
        table.push_back(std::move(recipe));
    }
    return table;
}
std::optional<std::pair<int,int>> find_mix(const std::vector<MixRecipe>& table, const std::vector<std::pair<int,int>>& chosen) {
    for (const auto& recipe : table) {
        if (recipe.parts.size() != chosen.size()) continue;
        auto left = chosen;
        bool ok = true;
        for (const auto& part : recipe.parts) {
            const auto found = std::find(left.begin(), left.end(), part);
            if (found == left.end()) { ok = false; break; }
            left.erase(found);
        }
        if (ok) return recipe.result;
    }
    return std::nullopt;
}

void ServiceSession::navigate(int direction) {
    if (busy || !message.empty() || mix_confirm) return;
    const int step = direction == 1 ? -1 : direction == 2 ? 1 : 0;
    if (!step) return;
    if (picking) {
        if (!picks.empty()) pick_sel = std::clamp(pick_sel + step, 0, int(picks.size()) - 1);
    } else if (page < 0) menu = std::clamp(menu + step, 0, 1);
    else cursor = std::clamp(cursor + step, 0, widgets() - 1);
}
void ServiceSession::back() {
    if (busy) return;
    if (!message.empty()) { message.clear(); question = false; }
    else if (mix_confirm) mix_confirm = false;
    else if (picking) picking = result_view = false;
    else done = true; // aa/ax.e(): leaving a page or the af menu closes the service
}
void ServiceSession::open_picker(std::vector<int> choices, int title, int empty_message) {
    if (choices.empty() && empty_message != 0) { message = {empty_message}; return; }
    picks = std::move(choices);
    pick_sel = 0; pick_title = title; picking = true; result_view = false;
}
void ServiceSession::set_refine_item(const ServiceInventory& inv, int ref) {
    item = ref;
    if (const auto* s = service_item(inv, ref)) { cost = s->refine * 100; need = s->refine + 1; }
}
void ServiceSession::pick(const ServiceInventory& inv, int ref) {
    picking = false;
    auto* s = service_item(inv, ref);
    if (!s) return;
    if (kind == ServiceKind::blacksmith && page == 0) {
        if (!s->identified) message = {11, 12};
        else set_refine_item(inv, ref);
    } else if (kind == ServiceKind::blacksmith) {
        if (s->identified) message = {21};
        else item = ref;
    } else if (page == 0 && cursor == 0) {
        if (!s->identified) message = {11, 13};
        else if (item_option(*s, find_catalog_item(inv.catalog, s->item_type, s->item_id)) != -1) message = {12, 13};
        else item = ref;
    } else if (page == 0) orb = ref;
    else {
        int others = 0;
        for (int i = 0; i < 3; ++i)
            if (i != cursor && mix[std::size_t(i)] >= 0)
                if (const auto* o = service_item(inv, mix[std::size_t(i)]); o && o->item_type == s->item_type && o->item_id == s->item_id) ++others;
        if (count_items(inv.bag, s->item_type, s->item_id) <= others) message = {31};
        else mix[std::size_t(cursor)] = ref;
    }
}
void ServiceSession::press(const ServiceInventory& inv) {
    const auto* chosen = service_item(inv, item);
    if (kind == ServiceKind::blacksmith && page == 0) {
        if (cursor == 0) {
            // at: g.a(false,0) lists the bag only; equipped pieces cannot be refined.
            auto choices = equipment_choices(inv, false, 0);
            choices.erase(std::remove_if(choices.begin(), choices.end(), [](int ref) { return ref >= 100; }), choices.end());
            return open_picker(std::move(choices), 3, -3936);
        }
        if (!chosen) message = {3};
        else if (chosen->refine >= limit_of(inv, *chosen)) message = {6, 7};
        else if (count_items(inv.bag, 11, 0) < need) message = {5};
        else if (inv.money < cost) message = {8};
        else { message = {9}; question = true; }
    } else if (kind == ServiceKind::blacksmith) {
        if (cursor == 0) return open_picker(equipment_choices(inv, false, -1), 3, -3935);
        if (!chosen) message = {3};
        else if (inv.money < 100) message = {8};
        else { message = {19}; question = true; }
    } else if (page == 0) {
        if (cursor == 0) return open_picker(equipment_choices(inv, true, 1), 3, 0);
        if (cursor == 1) return open_picker(bag_choices_of_type(inv.bag, 17), 4, 5);
        if (!chosen) message = {6};
        else if (!service_item(inv, orb)) message = {7};
        else if (inv.money < 500) message = {8};
        else { message = {9}; question = true; }
    } else {
        if (cursor < 3) return open_picker(mixable_choices(inv.bag), 21, 20);
        int chosen_count = 0;
        for (int ref : mix) chosen_count += ref >= 0;
        if (chosen_count < 2) message = {22, 23};
        else if (inv.money < 500) message = {24};
        else mix_confirm = true; // bo: names and "Mix cost" 500
    }
}
void ServiceSession::apply(const ServiceInventory& inv, const std::vector<MixRecipe>& mix_table, const std::function<int(int,int)>& roll) {
    message.clear(); question = false;
    if (kind == ServiceKind::blacksmith && page == 0) {
        auto* s = service_item(inv, item);
        if (!s) return;
        const int limit = limit_of(inv, *s), e = s->refine;
        remove_items(inv.bag, 11, 0, need, &item);
        inv.money -= cost;
        success = limit > 0 && 100 * (limit - e) / limit + 30 >= roll(1, 100);
        busy = 2;
    } else if (kind == ServiceKind::blacksmith) {
        auto* s = service_item(inv, item);
        if (!s) return;
        s->identified = true;
        inv.money -= 100;
        picks = {item}; pick_sel = 0; pick_title = 20; picking = result_view = true;
        item = -1;
    } else if (page == 0) {
        auto* s = service_item(inv, item);
        const auto* o = service_item(inv, orb);
        if (!s || !o) return;
        s->option = o->item_id;
        inv.money -= 500;
        remove_items(inv.bag, o->item_type, o->item_id, 1, &item);
        picks = {item}; pick_sel = 0; pick_title = 10; picking = result_view = true;
        item = orb = -1;
    } else {
        // k: no gold is taken here in the original, although bo shows the 500 cost.
        std::vector<std::pair<int,int>> parts;
        for (int ref : mix) if (const auto* s = service_item(inv, ref)) parts.emplace_back(s->item_type, s->item_id);
        const auto result = find_mix(mix_table, parts);
        const auto chosen = mix;
        std::array<std::pair<int,int>, 3> chosen_parts{};
        for (std::size_t i = 0; i < 3; ++i) {
            const auto* s = service_item(inv, chosen[i]);
            chosen_parts[i] = s ? std::pair<int,int>{s->item_type, s->item_id} : std::pair<int,int>{-1, -1};
        }
        for (const auto& part : parts) remove_items(inv.bag, part.first, part.second, 1);
        mix = {{-1, -1, -1}};
        if (!result) { message = {30}; return; }
        InventoryStack made{std::int8_t(result->first), std::int8_t(result->second), 1};
        if (const auto* item_data = find_catalog_item(inv.catalog, made.item_type, made.item_id))
            roll_equipment(made, *item_data, true, roll);
        const int index = add_item(inv.bag, made);
        if (index >= 0) { picks = {index}; pick_sel = 0; pick_title = 27; picking = result_view = true; }
        else {
            for (const auto& part : parts) add_item(inv.bag, {std::int8_t(part.first), std::int8_t(part.second), 1});
            message = {28, 29};
            // The original keeps its ingredient references; keep each choice that still names the same item.
            for (std::size_t i = 0; i < 3; ++i) {
                const auto* s = service_item(inv, chosen[i]);
                if (s && s->item_type == chosen_parts[i].first && s->item_id == chosen_parts[i].second) mix[i] = chosen[i];
            }
        }
    }
}
void ServiceSession::finish_refine(const ServiceInventory& inv) {
    if (!busy) return;
    busy = 0;
    auto* s = service_item(inv, item);
    if (!s) return;
    if (success) {
        ++s->refine;
        picks = {item}; pick_sel = 0; pick_title = 10; picking = result_view = true;
        set_refine_item(inv, item);
    } else {
        if (item >= 100) inv.equipment[std::size_t(item - 100)].reset();
        else inv.bag.erase(inv.bag.begin() + item);
        item = -1; cost = need = 0;
        autosave = true; // n.o()
        message = {26, 29};
    }
}
void ServiceSession::confirm(const ServiceInventory& inv, const std::vector<MixRecipe>& mix_table, const std::function<int(int,int)>& roll) {
    if (busy) return;
    if (!message.empty()) {
        if (question) apply(inv, mix_table, roll);
        else message.clear();
        return;
    }
    if (mix_confirm) { mix_confirm = false; message = {32}; question = true; return; }
    if (picking) {
        if (result_view) { picking = result_view = false; return; }
        if (pick_sel < int(picks.size())) pick(inv, picks[std::size_t(pick_sel)]);
        return;
    }
    if (page < 0) {
        page = menu; cursor = 0;
        // aa: entering the refining device first warns about destruction (blak 30..33).
        if (kind == ServiceKind::blacksmith && page == 0) message = {30, 31, 32, 33};
        return;
    }
    press(inv);
}
