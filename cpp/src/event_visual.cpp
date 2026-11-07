#include "event_visual.hpp"
#include <stdexcept>

EventVisualState make_event_visual_state(const EventGrid& event) {
    EventVisualState state;
    state.hero_emote = event.hero_emote;
    state.npc_emotes = event.npc_emotes;
    state.npc_emotes.resize(event.npcs.size() / 3);
    return state;
}

void apply_event_visual_command(EventVisualState& state, const EventCommand& command,int remaining_ticks) {
    switch (command.opcode) {
        case 22: state.world_hidden = true; return;
        case 23: state.world_hidden = false; return;
        case 24: state.flash_white=remaining_ticks>0 && remaining_ticks%2==1;return;
        case 25:
            if(remaining_ticks>0 && remaining_ticks<4) {
                const auto random_offset=[&]() {
                    auto& n=state.shake_rng;n^=n<<13;n^=n>>17;n^=n<<5;
                    return int(n%11)-5;
                };
                state.shake_x=random_offset();state.shake_y=random_offset();
            } else state.shake_x=state.shake_y=0;
            return;
        case 49: state.hero_emote = command.arg1; return;
        case 50:
            if (command.arg1 >= state.npc_emotes.size())
                throw std::out_of_range("EMO_NPC references missing actor");
            state.npc_emotes[command.arg1] = command.arg2;
            return;
        default: throw std::invalid_argument("not an event visual command");
    }
}
