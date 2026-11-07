#pragma once

#include "map.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct ShopItem {
    std::uint8_t type{};
    std::uint8_t id{};
    std::string name_key;
    std::string description_key;
    std::uint32_t price{};
    std::uint8_t shop_category{}; // 0..5, or 255 when ad.a() omits the type
    std::vector<std::uint8_t> class_data;
};

std::vector<ShopItem> parse_shop_catalog(const std::vector<std::uint8_t>& bytes);
std::vector<ShopItem> load_shop_catalog(const GameArchive& archive);

// Full itm/TT banks include items that are not available for purchase.
std::vector<ShopItem> parse_item_bank(const std::vector<std::uint8_t>& bytes, std::uint8_t type);
std::vector<ShopItem> load_item_catalog(const GameArchive& archive);
