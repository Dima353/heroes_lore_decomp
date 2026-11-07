#include "event_runtime.hpp"
#include "hero_movement.hpp"
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
int main() {
    try {
        GameMap map;
        map.width = map.height = 3;
        map.terrain.assign(9, 0);
        EventGrid event;
        event.overlay.assign(9, 0);
        for (std::uint8_t facing = 0; facing < 4; ++facing) {
            EventRuntime runtime(map, event, {1});
            const std::vector<std::uint8_t> actions{
                26, 8, 0, 7, 79, facing, 8, 1, 1, 9, 3, 0, 99, 0, 0};
            auto result = runtime.step(actions);
            require(result.status == EventStepStatus::audio_command && runtime.cursor() == 1,
                    "BGM must yield an audio request and advance");
            result = runtime.step(actions);
            require(result.status == EventStepStatus::map_transition && result.transition &&
                    result.transition->target_map_id == 79 &&
                    result.transition->destination_facing == facing &&
                    result.transition->hero_x == 1 && result.transition->hero_y == 1,
                    "map destination payload");
            require(runtime.ended() && runtime.step(actions).status == EventStepStatus::ended &&
                    (runtime.switch_bits()[0] & 8) == 0,
                    "map transition discards remaining source actions");
            HeroInput hero(map, event, {}, {});
            hero.place_on_tile(1, 1);
            hero.apply_script_command(6, static_cast<std::uint8_t>(facing + 1));
            require(hero.state().direction == facing + 1 && hero.state().motion_mode == 1,
                    "destination direction is facing plus one");
        }
        EventRuntime exit(map, event, {1});
        const std::vector<std::uint8_t> back{9,3,0,26,8,0,7,0,1,8,1,1,99,0,0};
        require(exit.step(back).status == EventStepStatus::applied, "exit switch mutation");
        require(exit.step(back).status == EventStepStatus::audio_command, "exit audio request");
        require(exit.step(back).status == EventStepStatus::map_transition &&
                (exit.switch_bits()[0] & 8) != 0, "exit preserves preceding mutations");
        EventRuntime unknown(map, event);
        require(unknown.step({98,0,0}).status == EventStepStatus::delegated && unknown.cursor() == 0,
                "unknown commands must still block");
        for (const std::uint8_t opcode : {27, 29}) {
            EventRuntime audio(map, event);
            require(audio.step({opcode,5,0,99,0,0}).status == EventStepStatus::audio_command &&
                    audio.cursor() == 1, "other confirmed audio commands advance");
        }
        std::cout << "Event transition checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
