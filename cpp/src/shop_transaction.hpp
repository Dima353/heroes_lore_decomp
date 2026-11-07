#pragma once
#include "event_trigger.hpp"
#include "item_catalog.hpp"

enum class ShopPurchaseResult { purchased, invalid_quantity, insufficient_money, inventory_full, invalid_item };

// ab.a(byte,byte) + g.a(ad,int). Failed purchases leave both arguments unchanged.
ShopPurchaseResult purchase_shop_item(const ShopItem& item, int quantity,
    std::vector<InventoryStack>& inventory, std::int64_t& money);

// ab.a(byte): Left/Right on the quantity. Buying wraps 1..99 (stackable types only); selling
// stops at 1 and at the stack count without wrapping.
int step_shop_quantity(int quantity, int direction, bool selling, bool stackable, int owned) noexcept;

enum class ShopSaleResult { sold, invalid_slot, invalid_quantity, protected_item, invalid_item, money_overflow };
bool can_sell_item_type(std::uint8_t type) noexcept;
std::int64_t shop_sale_value(const ShopItem& item, int quantity) noexcept;
// Operates on one selected slot, never another equal-type stack.
ShopSaleResult sell_shop_item(const ShopItem& item, std::size_t slot, int quantity,
    std::vector<InventoryStack>& inventory, std::int64_t& money);
