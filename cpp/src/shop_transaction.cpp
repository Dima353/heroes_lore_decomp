#include "shop_transaction.hpp"
#include "inventory_rules.hpp"
#include <algorithm>
#include <limits>

ShopPurchaseResult purchase_shop_item(const ShopItem& item, int quantity,
    std::vector<InventoryStack>& inventory, std::int64_t& money) {
    if (item.type >= stackable_item_types.size() || item.shop_category == 255)
        return ShopPurchaseResult::invalid_item;
    const bool stackable = is_stackable_item(item.type);
    if (quantity < 1 || quantity > inventory_stack_limit || (!stackable && quantity != 1))
        return ShopPurchaseResult::invalid_quantity;
    const auto cost = static_cast<std::int64_t>(item.price) * quantity;
    if (money < cost) return ShopPurchaseResult::insufficient_money;
    const auto type = static_cast<std::int8_t>(item.type);
    const auto id = static_cast<std::int8_t>(item.id < 128 ? int(item.id) : int(item.id) - 256);
    if (stackable) {
        const auto stack = std::find_if(inventory.begin(), inventory.end(), [&](const auto& entry) {
            return entry.item_type == type && entry.item_id == id &&
                   entry.count > 0 && entry.count <= inventory_stack_limit - quantity;
        });
        if (stack != inventory.end()) {
            stack->count += quantity;
            money -= cost;
            consolidate_stacks(inventory);
            return ShopPurchaseResult::purchased;
        }
    }
    if (inventory.size() >= inventory_slot_limit) return ShopPurchaseResult::inventory_full;
    // push_back may allocate; commit money only after it succeeds.
    inventory.push_back({type, id, quantity});
    money -= cost;
    consolidate_stacks(inventory);
    return ShopPurchaseResult::purchased;
}

int step_shop_quantity(int quantity, int direction, bool selling, bool stackable, int owned) noexcept {
    if (selling) {
        if (direction == 3 && quantity > 1) return quantity - 1;
        if (direction == 4 && quantity < owned) return quantity + 1;
        return quantity;
    }
    if (!stackable) return quantity;
    if (direction == 3) return quantity == 1 ? inventory_stack_limit : quantity - 1;
    if (direction == 4) return quantity == inventory_stack_limit ? 1 : quantity + 1;
    return quantity;
}
bool can_sell_item_type(std::uint8_t type) noexcept {
    return type < stackable_item_types.size() && (type < 18 || type > 21);
}
std::int64_t shop_sale_value(const ShopItem& item, int quantity) noexcept {
    // ab: multiply the whole quantity first, then integer divide by five.
    return quantity > 0 ? static_cast<std::int64_t>(item.price) * quantity / 5 : 0;
}
ShopSaleResult sell_shop_item(const ShopItem& item, std::size_t slot, int quantity,
    std::vector<InventoryStack>& inventory, std::int64_t& money) {
    if (slot >= inventory.size()) return ShopSaleResult::invalid_slot;
    const auto& stack = inventory[slot];
    if (item.type != static_cast<std::uint8_t>(stack.item_type) ||
        item.id != static_cast<std::uint8_t>(stack.item_id) || item.type >= stackable_item_types.size())
        return ShopSaleResult::invalid_item;
    if (!can_sell_item_type(item.type)) return ShopSaleResult::protected_item;
    if (quantity < 1 || quantity > inventory_stack_limit || quantity > stack.count ||
        (!is_stackable_item(item.type) && quantity != 1)) return ShopSaleResult::invalid_quantity;
    const auto value = shop_sale_value(item, quantity);
    if (money < 0 || money > std::numeric_limits<std::int64_t>::max() - value)
        return ShopSaleResult::money_overflow;
    inventory[slot].count -= quantity;
    if (inventory[slot].count == 0) inventory.erase(inventory.begin() + static_cast<std::ptrdiff_t>(slot));
    money += value;
    return ShopSaleResult::sold;
}
