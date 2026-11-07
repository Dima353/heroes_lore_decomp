#include "new_game.hpp"
#include "event_visual.hpp"
#include <iostream>
#include <stdexcept>

static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    try {
        const int starts[3][3]{{0,22,4},{60,5,36},{77,10,18}};
        const int weapons[3]{0,2,1};
        for (int c = 6; c <= 8; ++c) {
            const auto state = make_new_game_profile(c);
            check(state.map_id == starts[c-6][0] && state.x == starts[c-6][1] &&
                  state.y == starts[c-6][2] && state.direction == 2, "class starting position");
            check(state.money == 300 && state.switches.size() == 128 && state.switches[0] == 1,
                  "new game baseline");
            for (std::size_t i = 1; i < state.switches.size(); ++i) check(state.switches[i] == 0, "no story progress");
            for (const auto value : state.quests) check(value == 0, "no initial quest flags");
            check(state.equipment[0]->item_type == weapons[c-6] && state.equipment[0]->item_id == 0,
                  "starter weapon");
            check(state.equipment[1].has_value() == (c == 8), "only class 8 starts with shield");
            for (const auto& item : state.equipment) if (item)
                check(item->item_id == 0 && item->count == 1, "starter gear IDs and count");
        }
        EventVisualState visuals;
        visuals.npc_emotes.resize(2);
        apply_event_visual_command(visuals, {49,5,0});
        apply_event_visual_command(visuals, {50,1,4});
        check(visuals.hero_emote == 5 && visuals.npc_emotes[1] == 4, "actor emotes");
        apply_event_visual_command(visuals, {49,0,0});
        check(visuals.hero_emote == 0, "clear hero emote");
        apply_event_visual_command(visuals, {22,0,0});
        check(visuals.world_hidden, "hide world");
        apply_event_visual_command(visuals, {23,0,0});
        check(!visuals.world_hidden, "show world");
        bool rejected = false;
        try { apply_event_visual_command(visuals, {50,2,1}); }
        catch (const std::out_of_range&) { rejected = true; }
        check(rejected, "bad NPC index rejected");
        if (argc == 2) {
            GameArchive archive(argv[1]);
            auto map = load_map(archive, 0);
            auto event = load_event_grid(archive, "6", 0, map);
            const auto initial = make_new_game_profile(6);
            const auto setup = select_setup_group(event, initial.switches);
            check(setup && setup->first == 2, "new game setup 2");
            apply_setup_group(map, event, setup->second);
            check(!event.npc_visible[0] && !event.npc_visible[2] && !event.npc_visible[4],
                  "initial hidden actors");
            const auto match = find_event_trigger(event, 10, EventTriggerType::actor_contact,
                initial.switches, initial.quests);
            check(match && match->action_list == 9, "opening trigger at spawn");
            EventRuntime runtime(map, event, initial.switches, initial.quests);
            EventVisualState scene_visuals;
            scene_visuals.npc_emotes.resize(event.npcs.size() / 3);
            bool transitioned = false;
            for (int tick = 0; tick < 1000; ++tick) {
                const auto result = runtime.step(event.actions[9], true, true);
                check(result.status != EventStepStatus::delegated, "opening has no unsupported commands");
                if (result.status == EventStepStatus::visual_command)
                    apply_event_visual_command(scene_visuals, result.command);
                if (result.status == EventStepStatus::map_transition) {
                    check(result.transition && result.transition->target_map_id == 64 &&
                          result.transition->hero_x == 9 && result.transition->hero_y == 8,
                          "opening destination");
                    check((runtime.switch_bits()[54 / 8] & (1u << (54 % 8))) &&
                          (runtime.switch_bits()[87 / 8] & (1u << (87 % 8))), "opening progress flags");
                    transitioned = true;
                    break;
                }
            }
            check(transitioned, "opening reaches house without blocking");
        }
        std::cout << "New game checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
