#include "sprite.hpp"
#include "map.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>

int main(int argc, char** argv) {
    if (argc != 5) {
        std::cerr << "Usage: heroes_lore_frame <game.jar> <atlas-stem> <frame-index> <output.png>\n";
        return 2;
    }
    try {
        const auto index = std::stoull(argv[3]);
        const GameArchive archive(argv[1]);
        const auto png = load_sprite_frame_png(archive, argv[2], static_cast<std::size_t>(index));
        std::ofstream output(argv[4], std::ios::binary);
        if (!output) throw std::runtime_error("cannot open output PNG");
        output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        if (!output) throw std::runtime_error("failed to write output PNG");
        std::cout << "Wrote " << png.size() << " bytes from " << argv[2] << " frame " << index << '\n';
    } catch (const std::exception& error) {
        std::cerr << "heroes_lore_frame: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
