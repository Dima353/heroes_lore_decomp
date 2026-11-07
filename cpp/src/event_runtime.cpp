#include "event_runtime.hpp"
#include "inventory_rules.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

int signed_byte(std::uint8_t value) {
    return value >= 128 ? static_cast<int>(value) - 256 : value;
}

EventCommand command_at(const std::vector<std::uint8_t>& actions, std::size_t index) {
    const auto offset = index * 3;
    return {actions[offset], actions[offset + 1], actions[offset + 2]};
}
}

EventRuntime::EventRuntime(GameMap& map, EventGrid& event,
                           std::vector<std::uint8_t> switch_bits,
                           std::vector<std::uint8_t> quest_bits,
                           std::vector<InventoryStack> inventory,
                           std::int64_t money, HeroVitals vitals, GuardianRoster guardians, HeroProgress progress)
    : map_(map), event_(event), switch_bits_(std::move(switch_bits)),
      quest_bits_(std::move(quest_bits)), inventory_(std::move(inventory)),
      money_(std::max<std::int64_t>(0, money)), vitals_(vitals), guardians_(std::move(guardians)), progress_(progress) {
    switch_bits_.resize(std::max<std::size_t>(128, switch_bits_.size()), 0);
    quest_bits_.resize(std::max<std::size_t>(128, quest_bits_.size()), 0);
}

void EventRuntime::apply_switch(std::vector<std::uint8_t>& bits,
                                const EventCommand& command) {
    const auto id = static_cast<std::size_t>(command.arg1) |
        (static_cast<std::size_t>(command.arg2 & 0x0c) << 6);
    const auto operation = command.arg2 & 0x03;
    if (operation == 3) return;
    const auto byte_index = id / 8;
    if (byte_index >= bits.size()) bits.resize(byte_index + 1, 0);
    const auto mask = static_cast<std::uint8_t>(1u << (id % 8));
    if (operation == 0) bits[byte_index] |= mask;
    else if (operation == 1) bits[byte_index] &= static_cast<std::uint8_t>(~mask);
    else bits[byte_index] ^= mask;
}

EventStepResult EventRuntime::step(const std::vector<std::uint8_t>& actions,
                                   std::optional<bool> yes_no,
                                   bool dialogue_done, unsigned ticks) {
    if (actions.size() % 3 != 0)
        throw std::invalid_argument("event action data is not 3-byte aligned");
    const auto count = actions.size() / 3;
    if (ended_) return {EventStepStatus::ended, {}, cursor_, 0, std::nullopt, std::nullopt};
    if (cursor_ > count) throw std::out_of_range("event cursor outside action list");
    if (cursor_ == count) {
        ended_ = true;
        pending_ = Pending::none;
        return {EventStepStatus::ended, {}, cursor_, 0, std::nullopt, std::nullopt};
    }

    const auto command = command_at(actions, cursor_);
    can_consume_delegated_ = false;
    if (pending_ != Pending::none &&
        (pending_cursor_ != cursor_ || pending_opcode_ != command.opcode))
        throw std::logic_error("another event action is still pending");

    const auto advance = [&](std::size_t amount, EventStepStatus status,
                             int remaining = 0,
                             std::optional<MapTransitionRequest> transition = std::nullopt) {
        cursor_ += amount;
        pending_ = Pending::none;
        return EventStepResult{status, command, cursor_, remaining, transition, std::nullopt};
    };
    const auto branch = [&](int offset) {
        const auto target = static_cast<std::int64_t>(cursor_) + offset;
        if (target < 0 || static_cast<std::uint64_t>(target) > count)
            throw std::out_of_range("event branch target outside action list");
        cursor_ = static_cast<std::size_t>(target);
        pending_ = Pending::none;
        return EventStepResult{EventStepStatus::branch, command, cursor_, 0,
                               std::nullopt, std::nullopt};
    };
    const auto wait = [&](Pending kind, int remaining = 0) {
        if (pending_ == Pending::none) {
            pending_ = kind;
            pending_cursor_ = cursor_;
            pending_opcode_ = command.opcode;
        }
        return EventStepResult{EventStepStatus::wait, command, cursor_, remaining,
                               std::nullopt, std::nullopt};
    };

    switch (command.opcode) {
        case 1: // TALKTEXT: UI owns the text window and completion signal.
            if(skip_requested_) return advance(1,EventStepStatus::applied);
            if (pending_ == Pending::none || !dialogue_done) return wait(Pending::dialogue);
            return advance(1, EventStepStatus::applied);
        case 2: // YES/NO: Yes falls through; No uses signed arg2 as an offset.
            if (pending_ == Pending::none) skip_requested_ = false; // ah.a: opening the choice clears a:Z
            if (!yes_no) return wait(Pending::choice);
            return branch(*yes_no ? 1 : signed_byte(command.arg2));
        case 4: { // MV_DELAY's values in the supplied JARs are 1..30. Skip never shortens it (ah.d).
            if (pending_ == Pending::none) {
                remaining_ticks_ = signed_byte(command.arg1) - 1;
                if (remaining_ticks_ == 0) return advance(1, EventStepStatus::applied);
            } else if (remaining_ticks_ > 0) {
                remaining_ticks_ -= static_cast<int>(std::min<unsigned>(
                    ticks, static_cast<unsigned>(remaining_ticks_)));
                if (remaining_ticks_ == 0) return advance(1, EventStepStatus::applied);
            }
            return wait(Pending::delay, remaining_ticks_);
        }
        case 3: case 5: case 6:
        case 15: case 16: case 17:
            // The confirmed movement assignment is handed to the actor loop.
            cursor_ += 1;
            return {EventStepStatus::actor_command, command, cursor_, 0, std::nullopt, std::nullopt};
        case 7: // MAP_CHNG is immediately followed by MAP_HERO in all supplied JARs.
            if (cursor_ + 1 >= count || actions[(cursor_ + 1) * 3] != 8)
                throw std::runtime_error("MAP_CHNG is not followed by MAP_HERO");
            {
                if (command.arg2 > 3) throw std::runtime_error("invalid map destination facing");
                const auto hero = command_at(actions, cursor_ + 1);
                ended_ = true; // ah.i() requests the map; opcode 7 then calls ah.c().
                const MapTransitionRequest request{command.arg1, command.arg2,
                                                   hero.arg1, hero.arg2};
                return advance(2, EventStepStatus::map_transition, 0, request);
            }
        case 8: // ah.s: a lone MAP_HERO moves the hero to tile (arg1, arg2) on the current map.
            return advance(1, EventStepStatus::applied);
        case 9:
            apply_switch(switch_bits_, command);
            return advance(1, EventStepStatus::applied);
        case 10: { // MONEY: add the unsigned 16-bit value packed in arg1/arg2.
            const auto delta = (static_cast<std::uint32_t>(command.arg1) << 8) |
                               static_cast<std::uint32_t>(command.arg2);
            money_ += delta;
            return advance(1, EventStepStatus::applied);
        }
        case 12:
            add_event_experience(progress_,vitals_,guardians_,(unsigned(command.arg1)<<8)|command.arg2);
            return advance(1, EventStepStatus::applied);
        case 18:
            award_event_guardian(guardians_,command.arg1);
            return advance(1, EventStepStatus::applied);
        case 19: // COMBO: ah.l -> ao.d(byte) only ever raises the combo length.
            if (command.arg1 >= 1 && command.arg1 <= 5)
                progress_.max_combo = std::max(progress_.max_combo, int(command.arg1));
            return advance(1, EventStepStatus::applied);
        case 13: case 14: { // ah.m/n -> ao.b/d: unsigned16 additive HP/SP, clamp to maximum.
            const int amount = (int(command.arg1) << 8) | command.arg2;
            auto& value = command.opcode == 13 ? vitals_.hp : vitals_.sp;
            const int maximum = command.opcode == 13 ? vitals_.max_hp : vitals_.max_sp;
            value = std::clamp(value + amount, 0, maximum);
            return advance(1, EventStepStatus::applied);
        }
        case 11: { // ITEM must be paired with ITEM_NUM.
            if (cursor_ + 1 >= count || actions[(cursor_ + 1) * 3] != 21)
                throw std::runtime_error("ITEM is not followed by ITEM_NUM");
            const auto amount_command = command_at(actions, cursor_ + 1);
            const auto item_type = static_cast<std::int8_t>(signed_byte(command.arg1));
            const auto item_id = static_cast<std::int8_t>(signed_byte(command.arg2));
            const int amount = signed_byte(amount_command.arg1);
            if (pending_ == Pending::item_message) {
                if (!dialogue_done && !skip_requested_) return wait(Pending::item_message);
                // ah.g: closing the "full"/"no item" message calls ah.c(), ending the event.
                cursor_ += 2;
                ended_ = true;
                pending_ = Pending::none;
                return {EventStepStatus::ended, command, cursor_, 0, std::nullopt, std::nullopt};
            }
            if (item_type < 0 || static_cast<std::size_t>(item_type) >=
                                     stackable_item_types.size())
                throw std::out_of_range("ITEM references an invalid item type");
            if (amount > 0) {
                if (stackable_item_types[static_cast<std::size_t>(item_type)]) {
                    const auto stack = std::find_if(inventory_.begin(), inventory_.end(),
                        [&](const InventoryStack& candidate) {
                            return candidate.item_type == item_type &&
                                   candidate.item_id == item_id &&
                                   candidate.count + amount <= inventory_stack_limit;
                        });
                    if (stack != inventory_.end()) {
                        stack->count += amount;
                        consolidate_stacks(inventory_);
                        return advance(2, EventStepStatus::applied);
                    }
                }
                if (inventory_.size() >= inventory_slot_limit) {
                    item_message_string_index_ = 3938;
                    return wait(Pending::item_message);
                }
                const int added_count = stackable_item_types[static_cast<std::size_t>(item_type)]
                    ? amount : 1;
                inventory_.push_back({item_type, item_id, added_count});
                if (item_created) item_created(inventory_.back());
                consolidate_stacks(inventory_);
            } else if (amount < 0) {
                const int requested = -amount;
                int available = 0;
                for (const auto& stack : inventory_)
                    if (stack.item_type == item_type && stack.item_id == item_id)
                        available += stack.count;
                if (available < requested) {
                    item_message_string_index_ = 3939;
                    return wait(Pending::item_message);
                }
                int remaining = requested;
                for (auto stack = inventory_.begin(); stack != inventory_.end() && remaining > 0;) {
                    if (stack->item_type != item_type || stack->item_id != item_id) {
                        ++stack;
                        continue;
                    }
                    const int removed = std::min(stack->count, remaining);
                    stack->count -= removed;
                    remaining -= removed;
                    if (stack->count == 0) stack = inventory_.erase(stack);
                    else ++stack;
                }
            }
            return advance(2, EventStepStatus::applied);
        }
        case 30:
            apply_switch(quest_bits_, command);
            return advance(1, EventStepStatus::applied);
        case 24: case 25: { // ah.h: five flash phases / three random offset pairs, then a closing frame.
            // h() already runs in the dispatch frame, and Skip does not shorten it.
            if(pending_==Pending::none) {
                remaining_ticks_=command.opcode==24?5:3;
                wait(Pending::visual_effect);
            } else remaining_ticks_-=int(std::min<unsigned>(ticks,unsigned(remaining_ticks_)));
            if(!remaining_ticks_) return advance(1,EventStepStatus::visual_command);
            return {EventStepStatus::visual_command,command,cursor_,remaining_ticks_,std::nullopt,std::nullopt};
        }
        case 22: case 23: case 49: case 50:
            return advance(1, EventStepStatus::visual_command);
        case 26: case 27: case 29: // BGM_PLAY/BGM_STOP/SE_PLAY share bw's audio handoff.
            return advance(1, EventStepStatus::audio_command);
        case 31:
            return branch(signed_byte(command.arg1));
        case 32:
            return branch(-signed_byte(command.arg1));
        case 20: // GAMEOVER: the scenario ends and the ending sequence takes over.
            cursor_ += 1;
            ended_ = true;
            pending_ = Pending::none;
            return {EventStepStatus::ending, command, cursor_, 0, std::nullopt, std::nullopt};
        case 0: case 21: case 28: case 33: // CMD_IDLE, ITEM_NUM (read by ITEM), SYSBGM, SWI_MAP: no-ops in ah
            return advance(1, EventStepStatus::applied);
        case 34: case 35: case 36: case 37: case 38: case 39:
            // ah.w() detaches and stops the camera; MV_FO_HE re-attaches it to the hero,
            // MV_CA_MV starts a pan, MV_CA_XY centres tile (arg1, arg2). MV_FO_NP's NPC focus is unused.
            camera.follow = command.opcode == 34;
            camera.direction = command.opcode == 37 ? command.arg1 : 0;
            camera.speed = command.opcode == 37 ? command.arg2 : 0;
            if (command.opcode == 39) { camera.x = command.arg1 * 16; camera.y = command.arg2 * 16; }
            return advance(1, EventStepStatus::applied);
        case 40: { // CHG_OBJ replaces the image index of an existing object.
            const auto index = static_cast<std::size_t>(command.arg1);
            if (index >= event_.objects.size() / 5)
                throw std::out_of_range("CHG_OBJ references missing object");
            event_.objects[index * 5 + 4] = command.arg2;
            return advance(1, EventStepStatus::applied);
        }
        case 41: { // CHG_NPC changes ac.f, the NPC's visual code.
            const auto index = static_cast<std::size_t>(command.arg1);
            if (index >= event_.npcs.size() / 3)
                throw std::out_of_range("CHG_NPC references missing NPC");
            event_.npcs[index * 3 + 2] = command.arg2;
            return advance(1, EventStepStatus::applied);
        }
        case 42: { // CHGTL_XY + CHGTL_VA mutate terrain and collision grids together.
            if (cursor_ + 1 >= count || actions[(cursor_ + 1) * 3] != 43)
                throw std::runtime_error("CHGTL_XY is not followed by CHGTL_VA");
            const auto value = command_at(actions, cursor_ + 1);
            const auto x = static_cast<std::size_t>(command.arg1);
            const auto y = static_cast<std::size_t>(command.arg2);
            const auto index = y * map_.width + x;
            const auto cells = static_cast<std::size_t>(map_.width) * map_.height;
            if (x >= map_.width || y >= map_.height || index >= cells ||
                index >= map_.terrain.size() || index >= event_.overlay.size())
                throw std::out_of_range("tile change coordinate outside map");
            map_.terrain[index] = value.arg1;
            event_.overlay[index] = value.arg2;
            return advance(2, EventStepStatus::applied);
        }
        case 43:
            throw std::runtime_error("CHGTL_VA has no preceding CHGTL_XY");
        case 44: case 45: case 48: {
            const auto screen = command.opcode == 45 ? EventScreen::shop
                : command.opcode == 48 ? EventScreen::refiner
                                       : EventScreen::blacksmith;
            ++cursor_;
            pending_ = Pending::none;
            ended_ = true;
            return {EventStepStatus::screen_transition, command, cursor_, 0,
                    std::nullopt, screen};
        }
        case 46: case 47: { // HIDE_NPC / SHOW_NPC.
            const auto index = static_cast<std::size_t>(command.arg1);
            if (index >= event_.npc_visible.size())
                throw std::out_of_range("NPC visibility command references missing NPC");
            event_.npc_visible[index] = command.opcode == 47;
            return advance(1, EventStepStatus::applied);
        }
        case 99:
            cursor_ += 1;
            ended_ = true;
            pending_ = Pending::none;
            return {EventStepStatus::ended, command, cursor_, 0, std::nullopt, std::nullopt};
        default:
            can_consume_delegated_ = true;
            return {EventStepStatus::delegated, command, cursor_, 0, std::nullopt, std::nullopt};
    }
}

void EventCamera::tick() {
    static constexpr int dx[9]{0,0,0,-1,1,-1,1,-1,1}, dy[9]{0,-1,1,0,0,-1,-1,1,1}; // u.a / u.b
    if (follow || direction <= 0 || direction > 8) return;
    x += speed * dx[direction];
    y += speed * dy[direction];
}
void EventRuntime::consume_delegated() {
    if (!can_consume_delegated_ || ended_)
        throw std::logic_error("there is no delegated event command to consume");
    ++cursor_;
    can_consume_delegated_ = false;
}

bool event_step_continues_frame(const EventStepResult& result, const std::vector<std::uint8_t>& actions) {
    if (result.status != EventStepStatus::actor_command) return event_step_continues_frame(result);
    if (result.next_index * 3 >= actions.size()) return false;
    const int next = actions[result.next_index * 3];
    return (next >= 3 && next <= 6) || (next >= 15 && next <= 17) || (next >= 34 && next <= 39);
}

bool event_step_continues_frame(const EventStepResult& result) {
    if (result.command.opcode == 4 || result.command.opcode==24 || result.command.opcode==25) return false;
    return result.status == EventStepStatus::applied || result.status == EventStepStatus::branch ||
           result.status == EventStepStatus::audio_command || result.status == EventStepStatus::visual_command;
}
