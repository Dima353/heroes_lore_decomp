#pragma once
#include "event_runtime.hpp"

struct EventVisualState {
    bool world_hidden{};
    std::uint8_t hero_emote{}; // 0 clears, otherwise img/emoti frame id-1.
    std::vector<std::uint8_t> npc_emotes;
    bool flash_white{};
    int shake_x{},shake_y{};
    std::uint32_t shake_rng{0x6d2b79f5u};
};
EventVisualState make_event_visual_state(const EventGrid& event);
void apply_event_visual_command(EventVisualState& state, const EventCommand& command,int remaining_ticks=0);
