#include "hero_movement.hpp"

#include <iostream>
#include <stdexcept>

static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

int main() {
    try {
        // 5x5 open map with a wall at (2,0): turning mid-step must not skip collision checks.
        GameMap map;
        map.width = 5; map.height = 5;
        map.terrain.assign(25, 0); map.collision.assign(25, 0);
        EventGrid event;
        event.overlay.assign(25, 0);
        event.overlay[0 * 5 + 2] = 0xff; // signed -1 blocks
        HeroInput hero(map, event, {}, {});
        hero.place_on_tile(1, 2);
        hero.press(4);   // walk right
        hero.tick();     // half step: partial_x set
        check(hero.state().partial_x, "half step in progress");
        hero.apply_script_command(6, 1); // turn up mid-step (auto-facing / scripts)
        for (int i = 0; i < 12; ++i) hero.tick();
        const auto s = hero.state();
        check(!s.partial_x && !s.partial_y, "step completes before turning");
        check(s.direction == 1 && s.motion_mode == 1, "turn applied after the step, standing");
        hero.press(1);
        for (int i = 0; i < 12; ++i) hero.tick();
        check(hero.state().x_tile == 2 && hero.state().y_tile == 1, "walks up and stops below the wall");
        check(!(hero.state().x_tile == 2 && hero.state().y_tile == 0), "never enters the wall");
        std::cout << "Hero movement checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "hero movement test failed: " << error.what() << '\n';
        return 1;
    }
}
