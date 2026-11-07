#include "map.hpp"
#include "item_catalog.hpp"
#include "new_game.hpp"
#include "equipment.hpp"

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2 || argc > 5) {
        std::cerr << "Usage: heroes_lore_inspect <game.jar> [map-id] [zone] [switches-hex]\n"
                     "       heroes_lore_inspect <game.jar> --shop-catalog|--item-catalog\n"
                     "       heroes_lore_inspect <game.jar> --new-game [6|7|8]\n";
        return 2;
    }
    try {
        GameArchive archive(argv[1]);
        if (argc == 3 && (std::string(argv[2]) == "--shop-catalog" ||
                          std::string(argv[2]) == "--item-catalog")) {
            const bool full = std::string(argv[2]) == "--item-catalog";
            const auto catalog = full ? load_item_catalog(archive) : load_shop_catalog(archive);
            std::cout << (full ? "items=" : "shop_items=") << catalog.size() << '\n';
            for (const auto& item : catalog) {
                std::cout << "tab=" << unsigned(item.shop_category)
                          << " type=" << unsigned(item.type)
                          << " id=" << unsigned(item.id)
                          << " name_key=" << item.name_key
                          << " description_key=" << item.description_key
                          << " price=" << item.price
                          << " class_bytes=" << item.class_data.size();
                const auto gear = equipment_info(item);
                if (gear) std::cout << " base_" << (item.type <= 2 ? "attack=" : "defense=")
                    << unsigned(gear->power) << " option_count=" << int(gear->random_option_count);
                std::cout << '\n';
            }
            return 0;
        }
        if ((argc == 3 || argc == 4) && std::string(argv[2]) == "--new-game") {
            const auto state = make_new_game_profile(argc == 4 ? std::stoi(argv[3]) : 6);
            std::cout << "class=" << state.character_class << " map=" << state.map_id
                      << " spawn=" << state.x << ',' << state.y << " direction=" << state.direction
                      << " gold=" << state.money << " switch0=1 bag_slots=0\n";
            for (std::size_t slot = 0; slot < state.equipment.size(); ++slot)
                if (state.equipment[slot]) std::cout << "equipment_slot=" << slot
                    << " type=" << int(state.equipment[slot]->item_type)
                    << " id=" << int(state.equipment[slot]->item_id) << '\n';
            return 0;
        }
        const int map_id = argc >= 3 ? std::stoi(argv[2]) : 0;
        const std::string zone = argc >= 4 ? argv[3] : "6";
        auto map = load_map(archive, map_id);
        auto event = load_event_grid(archive, zone, map_id, map);
        std::vector<std::uint8_t> switches(128, 0);
        switches[0] = 1; // Original new-game baseline.
        if (argc == 5) {
            const auto supplied = parse_switch_hex(argv[4]);
            std::fill(switches.begin(), switches.end(), 0);
            std::copy(supplied.begin(), supplied.end(), switches.begin());
        }
        const auto setup = select_setup_group(event, switches);
        if (setup) apply_setup_group(map, event, setup->second);
        const auto hidden_objects = static_cast<std::size_t>(std::count(event.object_visible.begin(), event.object_visible.end(), false));
        const auto hidden_npcs = static_cast<std::size_t>(std::count(event.npc_visible.begin(), event.npc_visible.end(), false));
        std::cout << "map=" << map_id << " kind=" << unsigned(map.kind)
                  << " size=" << unsigned(map.width) << 'x' << unsigned(map.height)
                  << " terrain_cells=" << map.terrain.size()
                  << " overlay_cells=" << event.overlay.size()
                  << " objects=" << event.objects.size() / 5
                  << " npcs=" << event.npcs.size() / 3
                  << " enemies=" << event.enemies.size() / 3
                  << " condition_lists=" << event.conditions.size()
                  << " action_lists=" << event.actions.size()
                  << " setup_groups=" << event.setup_groups.size()
                  << " selected_setup=" << (setup ? setup->first : -1)
                  << " hidden_objects=" << hidden_objects
                  << " hidden_npcs=" << hidden_npcs
                  << " event_parsed_bytes=" << event.parsed_bytes
                  << " event_trailing_bytes=" << event.trailing_bytes << " zone=" << zone << '\n';
        const auto npcs = decode_npcs(event);
        const auto enemies = decode_enemies(event);
        std::size_t atlas_npcs = 0, animated_npcs = 0, unresolved_npcs = 0;
        for (const auto& npc : npcs) {
            if (npc.frame_index >= 0) ++atlas_npcs;
            else if (npc.resource_slot >= 0) ++animated_npcs;
            else ++unresolved_npcs;
        }
        std::size_t resolved_enemies = 0;
        for (const auto& enemy : enemies) if (enemy.resource_slot >= 0) ++resolved_enemies;
        std::cout << "npc_atlas=" << atlas_npcs << " npc_animated=" << animated_npcs
                  << " npc_unresolved=" << unresolved_npcs << " enemy_resources="
                  << resolved_enemies << '/' << enemies.size() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "heroes_lore_inspect: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
