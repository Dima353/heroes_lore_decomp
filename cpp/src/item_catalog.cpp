#include "item_catalog.hpp"

#include <stdexcept>
#include <utility>

namespace {
std::uint32_t read_u32_le(const std::vector<std::uint8_t>& bytes, std::size_t pos) {
    if (pos > bytes.size() || bytes.size() - pos < 4)
        throw std::runtime_error("truncated item catalog price");
    return std::uint32_t(bytes[pos]) | (std::uint32_t(bytes[pos + 1]) << 8) |
           (std::uint32_t(bytes[pos + 2]) << 16) | (std::uint32_t(bytes[pos + 3]) << 24);
}

std::uint8_t shop_category(std::uint8_t type) {
    // ad.a() routes types 0..2, 3, 5, 6, and 4 into shop tabs 1..5.
    switch (type) {
        case 0: case 1: case 2: return 1;
        case 3: return 2;
        case 5: return 3;
        case 6: return 4;
        case 4: return 5;
        case 7: case 9: case 10: return 0;
        default: return 255; // ad.a() does not put these types into any tab.
    }
}
ShopItem parse_fields(const std::vector<std::uint8_t>& bytes, std::size_t pos,
                      std::size_t end, std::uint8_t type, std::uint8_t id) {
    ShopItem item;
    item.type = type;
    item.id = id;
    if (pos >= end) throw std::runtime_error("missing item name index length");
    const auto name_length = static_cast<std::size_t>(bytes[pos++]);
    if (name_length > end - pos) throw std::runtime_error("item name index exceeds record");
    item.name_key.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos),
        bytes.begin() + static_cast<std::ptrdiff_t>(pos + name_length));
    pos += name_length;
    if (pos >= end) throw std::runtime_error("missing item description index length");
    const auto description_length = static_cast<std::size_t>(bytes[pos++]);
    if (description_length > end - pos) throw std::runtime_error("item description index exceeds record");
    item.description_key.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos),
        bytes.begin() + static_cast<std::ptrdiff_t>(pos + description_length));
    pos += description_length;
    if (end - pos < 4) throw std::runtime_error("item price exceeds record");
    item.price = read_u32_le(bytes, pos);
    pos += 4;
    item.shop_category = shop_category(item.type);
    item.class_data.assign(bytes.begin() + static_cast<std::ptrdiff_t>(pos),
        bytes.begin() + static_cast<std::ptrdiff_t>(end));
    return item;
}

}

std::vector<ShopItem> parse_shop_catalog(const std::vector<std::uint8_t>& bytes) {
    std::vector<ShopItem> items;
    std::size_t record = 0;
    while (record < bytes.size()) {
        const auto record_size = static_cast<std::size_t>(bytes[record]);
        if (record_size < 8 || record_size > bytes.size() - record - 1)
            throw std::runtime_error("invalid item catalog record length");
        const auto begin = record + 1;
        const auto end = begin + record_size;
        items.push_back(parse_fields(bytes, begin + 2, end, bytes[begin], bytes[begin + 1]));
        record = end;
    }
    return items;
}

std::vector<ShopItem> load_shop_catalog(const GameArchive& archive) {
    return parse_shop_catalog(archive.read_entry("itm/forshop"));
}

std::vector<ShopItem> parse_item_bank(const std::vector<std::uint8_t>& bytes, std::uint8_t type) {
    std::vector<ShopItem> items;
    std::size_t record = 0;
    while (record < bytes.size()) {
        const auto size = static_cast<std::size_t>(bytes[record]);
        if (size < 7 || size > bytes.size() - record - 1 || items.size() >= 128)
            throw std::runtime_error("invalid item bank record");
        const auto begin = record + 1;
        const auto end = begin + size;
        // ce.a(type,id) uses record ordinal as ID; ad.a(Z,bytes,1) skips
        // the stored tag. itm/02 record 24 has tag 1: do not use it as type.
        items.push_back(parse_fields(bytes, begin + 1, end, type,
                                    static_cast<std::uint8_t>(items.size())));
        record = end;
    }
    return items;
}

std::vector<ShopItem> load_item_catalog(const GameArchive& archive) {
    std::vector<ShopItem> items;
    for (unsigned type = 0; type < 24; ++type) {
        const auto path = "itm/" + std::string(type < 10 ? "0" : "") + std::to_string(type);
        auto bank = parse_item_bank(archive.read_entry(path), static_cast<std::uint8_t>(type));
        for (auto& item : bank) items.push_back(std::move(item));
    }
    return items;
}
