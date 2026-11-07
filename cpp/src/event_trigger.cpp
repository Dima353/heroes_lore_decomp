#include "event_trigger.hpp"

#include <stdexcept>

namespace {
std::int8_t signed_byte(std::uint8_t value) {
    return static_cast<std::int8_t>(value >= 128
        ? static_cast<int>(value) - 256 : value);
}

bool bit_is_set(const std::vector<std::uint8_t>& bits, unsigned id) {
    const auto byte_index = id / 8;
    if (byte_index >= bits.size()) return false;
    return ((bits[byte_index] >> (id % 8)) & 1u) != 0;
}

bool row_matches(const std::uint8_t* row,
                 const std::vector<std::uint8_t>& switches,
                 const std::vector<std::uint8_t>& quests,
                 const std::function<bool(std::uint8_t, std::uint8_t, std::uint8_t)>& has_item) {
    const auto flags = row[0];
    const unsigned check_a = ((flags & 0x0cu) << 6) | row[1];
    const unsigned check_b = ((flags & 0x03u) << 8) | row[2];
    const auto& bank_a = (flags & 0x20u) ? quests : switches;
    const auto& bank_b = (flags & 0x10u) ? quests : switches;
    if (!bit_is_set(bank_a, check_a) || !bit_is_set(bank_b, check_b)) return false;
    if (row[3] != 0xff) {
        if (!has_item || !has_item(row[3], row[4], row[5])) return false;
    }
    return true;
}
}

bool has_inventory_requirement(const std::vector<InventoryStack>& inventory,
                               std::uint8_t item_type, std::uint8_t item_id,
                               std::uint8_t minimum_count) {
    const auto type = signed_byte(item_type);
    const auto id = signed_byte(item_id);
    const auto minimum = signed_byte(minimum_count);
    for (const auto& stack : inventory)
        if (stack.item_type == type && stack.item_id == id && stack.count >= minimum)
            return true;
    return false;
}

std::optional<EventTriggerMatch> find_event_trigger(
    const EventGrid& event, int trigger_id, EventTriggerType type,
    const std::vector<std::uint8_t>& switch_bits,
    const std::vector<std::uint8_t>& quest_bits,
    const std::function<bool(std::uint8_t, std::uint8_t, std::uint8_t)>& has_item) {
    if (trigger_id < 1 || trigger_id > 127) return std::nullopt;
    const auto table_index = static_cast<std::size_t>(trigger_id - 1);
    if (table_index >= event.conditions.size()) return std::nullopt;
    const auto wanted_type = static_cast<std::uint8_t>(type);
    const auto& rows = event.conditions[table_index];
    if (rows.size() % 7 != 0)
        throw std::invalid_argument("event condition table is not 7-byte aligned");
    for (std::size_t i = 0; i < rows.size() / 7; ++i) {
        const auto* row = rows.data() + i * 7;
        if ((row[0] >> 6) != wanted_type) continue;
        if (!row_matches(row, switch_bits, quest_bits, has_item)) continue;
        // Original ah returns false immediately when the first matching condition
        // has no action list; it does not continue scanning later conditions.
        if (row[6] == 0xff) return std::nullopt;
        return EventTriggerMatch{i, row[6]};
    }
    return std::nullopt;
}

bool any_event_condition_met(
    const EventGrid& event, int trigger_id,
    const std::vector<std::uint8_t>& switch_bits,
    const std::vector<std::uint8_t>& quest_bits,
    const std::function<bool(std::uint8_t, std::uint8_t, std::uint8_t)>& has_item) {
    if (trigger_id < 1 || trigger_id > 127) return false;
    const auto table_index = static_cast<std::size_t>(trigger_id - 1);
    if (table_index >= event.conditions.size()) return false;
    const auto& rows = event.conditions[table_index];
    for (std::size_t i = 0; i + 7 <= rows.size(); i += 7)
        if (row_matches(rows.data() + i, switch_bits, quest_bits, has_item)) return true;
    return false;
}
