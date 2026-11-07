#pragma once

#include <cstdint>
#include <string>
#include <vector>

class GameArchive;

struct EifLayer {
    std::int8_t x{};
    std::int8_t y{};
    std::uint8_t variant{};
    std::uint8_t atlas_frame{};
};

using EifFrame = std::vector<EifLayer>;
using EifAnimation = std::vector<EifFrame>;

EifAnimation parse_eif(const std::vector<std::uint8_t>& bytes);
EifAnimation load_eif(const GameArchive& archive, const std::string& path);
