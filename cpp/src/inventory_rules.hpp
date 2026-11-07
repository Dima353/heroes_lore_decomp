#pragma once
#include <array>
#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <vector>

inline constexpr std::size_t inventory_slot_limit = 30;
inline constexpr int inventory_stack_limit = 99;
// ad.b: true means that identical type/ID items may share a stack.
inline constexpr std::array<bool, 24> stackable_item_types{{
    false, false, false, false, false, false, false, true,
    true, true, true, true, true, true, true, true,
    true, true, false, false, false, false, true, true
}};
inline bool is_stackable_item(std::uint8_t type) noexcept {
    return type < stackable_item_types.size() && stackable_item_types[type];
}

// g.e(): after an item is added, later stacks of the same stackable item are poured into the
// earlier one up to 99; emptied stacks leave the bag (g.d() compaction).
template<class Stack> void consolidate_stacks(std::vector<Stack>& bag) {
    for (std::size_t i = 0; i + 1 < bag.size(); ++i) {
        auto& first = bag[i];
        if (!is_stackable_item(std::uint8_t(first.item_type)) || first.count >= inventory_stack_limit) continue;
        for (std::size_t j = i + 1; j < bag.size() && first.count < inventory_stack_limit;) {
            auto& other = bag[j];
            if (other.item_type != first.item_type || other.item_id != first.item_id) { ++j; continue; }
            const int moved = std::min(other.count, inventory_stack_limit - first.count);
            first.count += moved; other.count -= moved;
            if (other.count == 0) bag.erase(bag.begin() + std::ptrdiff_t(j)); else ++j;
        }
    }
}
