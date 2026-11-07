#pragma once

#include "event_trigger.hpp"
#include "map.hpp"
#include "hero_vitals.hpp"
#include "guardian.hpp"
#include "hero_progress.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

struct EventCommand {
    std::uint8_t opcode{};
    std::uint8_t arg1{};
    std::uint8_t arg2{};
};

struct MapTransitionRequest {
    std::uint8_t target_map_id{};
    std::uint8_t destination_facing{}; // 0..3; n.f() converts it to actor direction 1..4.
    std::uint8_t hero_x{};
    std::uint8_t hero_y{};
};

enum class EventStepStatus {
    applied,
    wait,
    branch,
    ended,
    actor_command,
    audio_command,
    visual_command,
    map_transition,
    screen_transition,
    ending,      // GAMEOVER 20: ah.c(), then as state 12 (story, staff roll, Hardcore mode)
    delegated
};

enum class EventScreen { shop, refiner, blacksmith };

struct EventStepResult {
    EventStepStatus status{EventStepStatus::applied};
    EventCommand command{};
    std::size_t next_index{};
    int remaining_ticks{};
    std::optional<MapTransitionRequest> transition;
    std::optional<EventScreen> screen;
};

// Whether the SDL loop can execute the next command before presenting a frame.
bool event_step_continues_frame(const EventStepResult& result);
// ah.a state 1 -> ah.d: consecutive movement commands (3/5/6, 15-17, camera 34-39) and the
// MV_DELAY after them run in the frame that dispatched the first one; any other command
// waits for the next frame. `actions` is the running action (3 bytes per command).
bool event_step_continues_frame(const EventStepResult& result, const std::vector<std::uint8_t>& actions);

// Executes the confirmed synchronous/event-control subset of ah's three-byte
// action stream. Commands owned by other game systems are returned explicitly.
// ah camera fields e/b/c/d: the camera follows the hero until MV_FO_NO/MV_CA_* detach it.
// Positions are hero-style pixel coordinates (tile * 16) of the point kept at the screen centre.
struct EventCamera {
    bool follow{true};
    int x{}, y{};
    int direction{}, speed{}; // MV_CA_MV: ah.b() moves speed px per tick along u.a/u.b
    void tick();
};

class EventRuntime {
public:
    EventRuntime(GameMap& map, EventGrid& event,
                 std::vector<std::uint8_t> switch_bits = {},
                 std::vector<std::uint8_t> quest_bits = {},
                 std::vector<InventoryStack> inventory = {},
                 std::int64_t money = 300, HeroVitals vitals = {}, GuardianRoster guardians = {}, HeroProgress progress = {});

    EventStepResult step(const std::vector<std::uint8_t>& actions,
                         std::optional<bool> yes_no = std::nullopt,
                         bool dialogue_done = false,
                         unsigned ticks = 1);
    void consume_delegated();
    // ah.a:Z: texts are skipped and the frame limiter is bypassed (bs.b) until the next YES/NO.
    void request_skip() noexcept { skip_requested_=true; }
    bool skipping() const noexcept { return skip_requested_; }
    // ah ITEM: new equipment is created with random bonuses and forced identified.
    std::function<void(InventoryStack&)> item_created;
    EventCamera camera;

    std::size_t cursor() const noexcept { return cursor_; }
    bool ended() const noexcept { return ended_; }
    const std::vector<std::uint8_t>& switch_bits() const noexcept { return switch_bits_; }
    const std::vector<std::uint8_t>& quest_bits() const noexcept { return quest_bits_; }
    const std::vector<InventoryStack>& inventory() const noexcept { return inventory_; }
    std::int64_t money() const noexcept { return money_; }
    const HeroVitals& vitals() const noexcept { return vitals_; }
    void set_vitals(const HeroVitals& vitals) noexcept { vitals_ = vitals; } // cf ticks during scripted control
    const GuardianRoster& guardians() const noexcept { return guardians_; }
    const HeroProgress& progress() const noexcept { return progress_; }
    std::size_t item_message_string_index() const noexcept {
        return item_message_string_index_;
    }

private:
    enum class Pending { none, dialogue, choice, delay, item_message, visual_effect };

    GameMap& map_;
    EventGrid& event_;
    std::vector<std::uint8_t> switch_bits_;
    std::vector<std::uint8_t> quest_bits_;
    std::vector<InventoryStack> inventory_;
    std::int64_t money_{};
    HeroVitals vitals_;
    GuardianRoster guardians_;
    HeroProgress progress_;
    std::size_t item_message_string_index_{3939};
    std::size_t cursor_{};
    std::size_t pending_cursor_{};
    std::uint8_t pending_opcode_{};
    Pending pending_{Pending::none};
    int remaining_ticks_{};
    bool can_consume_delegated_{};
    bool ended_{};
    bool skip_requested_{};

    void apply_switch(std::vector<std::uint8_t>& bits, const EventCommand& command);
};
