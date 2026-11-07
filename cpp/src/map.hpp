#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <optional>
#include <utility>
#include <vector>

#include <minizip/unzip.h>

struct GameMap {
    std::uint8_t kind{};
    std::uint8_t width{};
    std::uint8_t height{};
    std::vector<std::uint8_t> terrain;
    std::vector<std::uint8_t> collision;
};

struct MapEntity {
    std::uint8_t x{};
    std::uint8_t y{};
    std::uint8_t code{};
    std::uint8_t image{};
    int resource_slot{-1};
    int frame_index{-1};
    std::int8_t offset_x{};
    std::int8_t offset_y{};
    bool partial_x{};
    bool partial_y{};
};

struct EventGrid {
    std::vector<std::uint8_t> overlay;
    std::vector<std::uint8_t> object_resources;
    std::vector<std::uint8_t> objects;
    std::vector<std::uint8_t> npc_resources;
    std::vector<std::uint8_t> npcs;
    std::vector<std::uint8_t> enemy_resources;
    std::vector<std::uint8_t> enemies;
    std::vector<std::uint8_t> face_resources;
    std::vector<std::vector<std::uint8_t>> conditions;
    std::vector<std::vector<std::uint8_t>> actions;
    std::vector<std::vector<std::uint8_t>> strings;
    std::vector<std::uint8_t> setup_conditions;
    std::vector<std::vector<std::uint8_t>> setup_groups;
    std::vector<bool> object_visible;
    std::vector<bool> npc_visible;
    std::uint8_t hero_emote{}; // setup 111; 0 clears, otherwise frame+1.
    std::vector<std::uint8_t> npc_emotes; // setup 112, actor index -> frame+1.
    std::size_t parsed_bytes{};
    std::size_t trailing_bytes{};
};

struct LanguagePack;

class GameArchive {
public:
    explicit GameArchive(const std::string& jar);
    ~GameArchive();
    GameArchive(const GameArchive&) = delete;
    GameArchive& operator=(const GameArchive&) = delete;
    GameArchive(GameArchive&& other) noexcept;
    GameArchive& operator=(GameArchive&& other) noexcept;
    std::vector<std::uint8_t> read_entry(const std::string& name) const;
    // Optional resources: probe without throwing (exceptions abort the Switch build here).
    bool contains(const std::string& name) const;
    // Built-in language pack: its fonts/, lang/ and fr-FR/ entries replace the JAR's (nullptr = JAR text).
    void set_language_pack(const struct LanguagePack* pack) { pack_ = pack; }
    bool has_language_pack() const { return pack_ != nullptr; }
private:
    unzFile handle_{};
    const struct LanguagePack* pack_{};
    std::string path_;
};

std::vector<std::uint8_t> read_zip_entry(const std::string& jar, const std::string& name);
GameMap load_map(const std::string& jar, int map_id);
GameMap load_map(const GameArchive& archive, int map_id);
EventGrid parse_event_grid(const std::vector<std::uint8_t>& bytes, const GameMap& map);
EventGrid load_event_grid(const std::string& jar, const std::string& zone, int map_id, const GameMap& map);
EventGrid load_event_grid(const GameArchive& archive, const std::string& zone, int map_id, const GameMap& map);
std::vector<std::uint8_t> parse_switch_hex(const std::string& hex);
std::vector<MapEntity> decode_objects(const EventGrid& event);
std::vector<MapEntity> decode_npcs(const EventGrid& event);
std::vector<MapEntity> decode_enemies(const EventGrid& event);
std::optional<std::pair<int, std::vector<std::uint8_t>>> select_setup_group(
    const EventGrid& event, const std::vector<std::uint8_t>& switch_bits);
void apply_setup_group(GameMap& map, EventGrid& event, const std::vector<std::uint8_t>& commands);
