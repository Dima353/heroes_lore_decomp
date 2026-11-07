#include "eif.hpp"

#include "map.hpp"

#include <stdexcept>
#include <utility>

namespace {
std::int8_t signed_byte(std::uint8_t value) {
    return static_cast<std::int8_t>(value >= 128 ? static_cast<int>(value) - 256 : value);
}
}

EifAnimation parse_eif(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) throw std::runtime_error("empty EIF file");
    std::size_t pos = 0;
    const auto frame_count = static_cast<std::size_t>(bytes[pos++]);
    EifAnimation animation;
    animation.reserve(frame_count);
    for (std::size_t frame_index = 0; frame_index < frame_count; ++frame_index) {
        if (pos >= bytes.size()) throw std::runtime_error("EIF frame is missing layer count");
        const auto layer_count = static_cast<std::size_t>(bytes[pos++]);
        if (layer_count > (bytes.size() - pos) / 4)
            throw std::runtime_error("EIF layer records exceed file boundary");
        EifFrame frame;
        frame.reserve(layer_count);
        for (std::size_t i = 0; i < layer_count; ++i) {
            frame.push_back({signed_byte(bytes[pos]), signed_byte(bytes[pos + 1]),
                             bytes[pos + 2], bytes[pos + 3]});
            pos += 4;
        }
        animation.push_back(std::move(frame));
    }
    if (pos != bytes.size())
        throw std::runtime_error("trailing bytes after EIF frames: " + std::to_string(bytes.size() - pos));
    return animation;
}

EifAnimation load_eif(const GameArchive& archive, const std::string& path) {
    return parse_eif(archive.read_entry(path));
}
