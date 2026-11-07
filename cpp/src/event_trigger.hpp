#pragma once

#include "map.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

enum class EventTriggerType : std::uint8_t {
    actor_contact = 0,
    hero_on_tile = 1,
    hero_facing_tile = 2,
    hero_stop_facing_tile = 3,
};

struct EventTriggerMatch {
    std::size_t condition_index{};
    std::size_t action_list{};
};

struct InventoryStack {
    std::int8_t item_type{};
    std::int8_t item_id{};
    int count{};
    bool identified{true}; // Purchased/event gear is forced identified by ab/ah.
    // ad.a() bytes 4..9: per-instance equipment state.
    std::int8_t refine{};               // e.e: blacksmith refinement level
    std::array<std::int8_t,4> bonus{};  // e.j: Power / HP / Agility / Will bonuses
    std::int8_t option{-2};             // t.c attribute: -1 none, -2 = catalog default
};

// Mirrors g.a(byte, byte, byte): exact signed-byte item keys and count >= minimum.
bool has_inventory_requirement(const std::vector<InventoryStack>& inventory,
                               std::uint8_t item_type, std::uint8_t item_id,
                               std::uint8_t minimum_count);

// Evaluates the first matching 7-byte condition row exactly as ah.a(mode, id).
// The inventory callback receives the raw item triplet from bytes 3..5.
std::optional<EventTriggerMatch> find_event_trigger(
    const EventGrid& event, int trigger_id, EventTriggerType type,
    const std::vector<std::uint8_t>& switch_bits,
    const std::vector<std::uint8_t>& quest_bits,
    const std::function<bool(std::uint8_t, std::uint8_t, std::uint8_t)>& has_item = {});


// ah.a(byte): true when any condition row of the trigger holds, whatever its trigger type
// or action list (the minimap marks such cells).
bool any_event_condition_met(
    const EventGrid& event, int trigger_id,
    const std::vector<std::uint8_t>& switch_bits,
    const std::vector<std::uint8_t>& quest_bits,
    const std::function<bool(std::uint8_t, std::uint8_t, std::uint8_t)>& has_item = {});
