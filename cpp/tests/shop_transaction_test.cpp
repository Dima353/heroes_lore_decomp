#include "shop_transaction.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

static void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
static ShopItem item(unsigned type, unsigned id, std::uint32_t price) {
    ShopItem result;
    result.type = static_cast<std::uint8_t>(type);
    result.id = static_cast<std::uint8_t>(id);
    result.price = price;
    return result;
}
int main() {
    try {
        auto potion = item(7, 0, 100);
        std::vector<InventoryStack> bag;
        std::int64_t gold = 300;
        require(purchase_shop_item(potion, 3, bag, gold) == ShopPurchaseResult::purchased,
                "exact balance purchase");
        require(gold == 0 && bag.size() == 1 && bag[0].count == 3, "purchase commits both values");
        require(purchase_shop_item(potion, 1, bag, gold) == ShopPurchaseResult::insufficient_money,
                "insufficient funds rejected");
        require(gold == 0 && bag[0].count == 3, "funds failure is atomic");
        gold = 100000;
        bag.assign(30, {0, 0, 1});
        bag[12] = {7, 0, 98};
        require(purchase_shop_item(potion, 1, bag, gold) == ShopPurchaseResult::purchased,
                "full bag can merge a stack");
        require(bag.size() == 30 && bag[12].count == 99 && gold == 99900, "merge reaches 99");
        require(purchase_shop_item(potion, 1, bag, gold) == ShopPurchaseResult::inventory_full,
                "full stack and full bag rejected");
        require(bag[12].count == 99 && gold == 99900, "capacity failure is atomic");
        bag[12].count = 98;
        bag[13] = {7, 0, 98};
        require(purchase_shop_item(potion, 2, bag, gold) == ShopPurchaseResult::inventory_full,
                "purchase cannot be split between stacks");
        bag.pop_back();
        require(purchase_shop_item(potion, 2, bag, gold) == ShopPurchaseResult::purchased,
                "overflow uses new slot");
        // g.e(): the new stack of 2 is poured into the earlier potion stacks (98 -> 99, 98 -> 99).
        require(bag.size() == 29 && bag[12].count == 99 && bag[13].count == 99,
                "stacks of the same item are consolidated after a purchase");
        bag.clear();
        require(purchase_shop_item(potion, 0, bag, gold) == ShopPurchaseResult::invalid_quantity,
                "zero quantity rejected");
        require(purchase_shop_item(potion, 100, bag, gold) == ShopPurchaseResult::invalid_quantity,
                "quantity above 99 rejected");
        auto sword = item(0, 1, 400);
        require(purchase_shop_item(sword, 2, bag, gold) == ShopPurchaseResult::invalid_quantity,
                "equipment quantity restricted to one");
        require(purchase_shop_item(sword, 1, bag, gold) == ShopPurchaseResult::purchased &&
                purchase_shop_item(sword, 1, bag, gold) == ShopPurchaseResult::purchased &&
                bag.size() == 2, "equipment uses separate slots");
        auto costly = item(7, 0, std::numeric_limits<std::uint32_t>::max());
        gold = static_cast<std::int64_t>(costly.price) * 99;
        bag.clear();
        require(purchase_shop_item(costly, 99, bag, gold) == ShopPurchaseResult::purchased &&
                gold == 0 && bag[0].count == 99, "price multiplication does not overflow");
        // Sale must address the selected slot, not the first matching stack.
        bag = {{7, 0, 4}, {7, 0, 8}};
        gold = 0;
        require(sell_shop_item(potion, 1, 3, bag, gold) == ShopSaleResult::sold &&
                bag[0].count == 4 && bag[1].count == 5 && gold == 60,
                "partial sale changes selected stack only");
        require(sell_shop_item(potion, 1, 5, bag, gold) == ShopSaleResult::sold &&
                bag.size() == 1 && bag[0].count == 4 && gold == 160,
                "last sale erases slot");
        require(sell_shop_item(potion, 0, 5, bag, gold) == ShopSaleResult::invalid_quantity &&
                bag[0].count == 4 && gold == 160, "oversale rejected atomically");
        require(sell_shop_item(potion, 9, 1, bag, gold) == ShopSaleResult::invalid_slot,
                "missing sale slot rejected");
        auto other = item(7, 1, 100);
        require(sell_shop_item(other, 0, 1, bag, gold) == ShopSaleResult::invalid_item &&
                bag[0].count == 4 && gold == 160, "mismatched metadata rejected");
        for (unsigned type = 18; type <= 21; ++type) {
            bag = {{static_cast<std::int8_t>(type), 0, 1}};
            require(sell_shop_item(item(type, 0, 100), 0, 1, bag, gold) == ShopSaleResult::protected_item &&
                    bag.size() == 1 && bag[0].count == 1 && gold == 160,
                    "quest types 18..21 protected");
        }
        auto cheap = item(7, 0, 3);
        bag = {{7, 0, 2}};
        gold = 0;
        require(sell_shop_item(cheap, 0, 2, bag, gold) == ShopSaleResult::sold &&
                gold == 1 && bag.empty(), "round only after quantity multiplication");
        bag = {{7, 0, 1}};
        gold = std::numeric_limits<std::int64_t>::max();
        require(sell_shop_item(potion, 0, 1, bag, gold) == ShopSaleResult::money_overflow &&
                bag[0].count == 1 && gold == std::numeric_limits<std::int64_t>::max(),
                "sale overflow rejected atomically");
        // Synthetic full-bank fixture deliberately uses a different stored tag.
        const std::vector<std::uint8_t> bank{9, 1, 1, '2', 1, '3', 0x90, 1, 0, 0};
        const auto parsed = parse_item_bank(bank, 2);
        require(parsed.size() == 1 && parsed[0].type == 2 && parsed[0].id == 0 &&
                parsed[0].name_key == "2" && parsed[0].price == 400,
                "full bank uses bank type and ordinal ID");
        bool rejected = false;
        try { parse_item_bank({9, 1, 1, '2'}, 2); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "truncated full bank rejected");
        std::cout << "Shop transaction checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
