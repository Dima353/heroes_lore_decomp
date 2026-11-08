#include "map.hpp"
#include "language_pack.hpp"
#include "platform.hpp"

#include <minizip/unzip.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

// Outside the unnamed namespace: inside it the names are mangled and never meet the bin2s symbols.
#if HEROES_LORE_NDS
// heroes_lore_jar.nds links the game JAR into the program (bin2s game_jar); it is read in place.
extern "C" const std::uint8_t game_jar[] __attribute__((weak));
extern "C" const std::uint32_t game_jar_size __attribute__((weak));
#endif
namespace {
#if HEROES_LORE_3DS || HEROES_LORE_NDS
// The 3DS SD card (and a DS flash cart through DLDI) is very slow on the many small reads and seeks minizip makes (each entry lookup
// walks the central directory): the JAR (under 1 MB) is read once and served from memory.
using JarBytes = std::shared_ptr<const std::vector<std::uint8_t>>;
struct MemoryJar { JarBytes keep; const std::uint8_t* bytes{}; ZPOS64_T size{}, position{}; };
JarBytes jar_bytes(const std::string& path) {
    static std::mutex lock;
    static std::string cached_path;
    static JarBytes cached;
    std::lock_guard<std::mutex> guard(lock);
    if (cached && cached_path == path) return cached;
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return nullptr;
    std::vector<std::uint8_t> bytes;
    if (std::fseek(file, 0, SEEK_END) == 0) { // exact size: no growth slack (the DS has 4 MB)
        const long size = std::ftell(file);
        if (size > 0) bytes.reserve(static_cast<std::size_t>(size));
        std::fseek(file, 0, SEEK_SET);
    }
    std::vector<std::uint8_t> chunk(64u << 10);
    for (std::size_t count; (count = std::fread(chunk.data(), 1, chunk.size(), file)) > 0;)
        bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(count));
    std::fclose(file);
    cached = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
    cached_path = path;
    return cached;
}
voidpf ZCALLBACK memory_open(voidpf, const void* name, int) {
#if HEROES_LORE_NDS
    if (std::strcmp(static_cast<const char*>(name), "embedded:/Heroes_Lore.jar") == 0)
        return &game_jar_size ? new MemoryJar{nullptr, game_jar, game_jar_size} : nullptr;
#endif
    auto data = jar_bytes(static_cast<const char*>(name));
    if (!data) return nullptr;
    const auto* bytes = data->data();
    const ZPOS64_T size = data->size();
    return new MemoryJar{std::move(data), bytes, size};
}
uLong ZCALLBACK memory_read(voidpf, voidpf stream, void* buffer, uLong size) {
    auto* jar = static_cast<MemoryJar*>(stream);
    const auto total = jar->size;
    const auto count = static_cast<uLong>(jar->position >= total ? 0 : std::min<ZPOS64_T>(size, total - jar->position));
    std::memcpy(buffer, jar->bytes + jar->position, count);
    jar->position += count;
    return count;
}
uLong ZCALLBACK memory_write(voidpf, voidpf, const void*, uLong) { return 0; }
ZPOS64_T ZCALLBACK memory_tell(voidpf, voidpf stream) { return static_cast<MemoryJar*>(stream)->position; }
long ZCALLBACK memory_seek(voidpf, voidpf stream, ZPOS64_T offset, int origin) {
    auto* jar = static_cast<MemoryJar*>(stream);
    const ZPOS64_T base = origin == ZLIB_FILEFUNC_SEEK_SET ? 0 : origin == ZLIB_FILEFUNC_SEEK_CUR ? jar->position : jar->size;
    const ZPOS64_T target = base + offset; // a negative offset wraps around, as minizip expects
    if (target > jar->size) return -1;
    jar->position = target;
    return 0;
}
int ZCALLBACK memory_close(voidpf, voidpf stream) { delete static_cast<MemoryJar*>(stream); return 0; }
int ZCALLBACK memory_error(voidpf, voidpf) { return 0; }
unzFile open_jar(const std::string& jar) {
    zlib_filefunc64_def functions{memory_open, memory_read, memory_write, memory_tell, memory_seek,
                                  memory_close, memory_error, nullptr};
    return unzOpen2_64(jar.c_str(), &functions);
}
#else
unzFile open_jar(const std::string& jar) { return unzOpen64(jar.c_str()); }
#endif
}

GameArchive::GameArchive(const std::string& jar) : handle_(open_jar(jar)), path_(jar) {
    if (!handle_) throw std::runtime_error("Cannot open JAR: " + jar);
}

GameArchive::~GameArchive() { if (handle_) unzClose(handle_); }

GameArchive::GameArchive(GameArchive&& other) noexcept
    : handle_(other.handle_), pack_(other.pack_), path_(std::move(other.path_)) { other.handle_ = nullptr; }

GameArchive& GameArchive::operator=(GameArchive&& other) noexcept {
    if (this != &other) {
        if (handle_) unzClose(handle_);
        handle_ = other.handle_;
        pack_ = other.pack_;
        path_ = std::move(other.path_);
        other.handle_ = nullptr;
    }
    return *this;
}

bool GameArchive::contains(const std::string& name) const {
    if (pack_ && find_language_pack_file(*pack_, name)) return true;
    return handle_ && unzLocateFile(handle_, name.c_str(), 1) == UNZ_OK;
}

std::vector<std::uint8_t> GameArchive::read_entry(const std::string& name) const {
    if (!handle_) throw std::runtime_error("JAR is closed: " + path_);
    if (pack_)
        if (const auto* file = find_language_pack_file(*pack_, name)) return {file->data, file->data + file->size};
    auto zip = handle_;
    if (unzLocateFile(handle_, name.c_str(), 1) != UNZ_OK) {
        throw std::runtime_error("Entry not found in JAR: " + name);
    }
    unz_file_info64 info{};
    if (unzGetCurrentFileInfo64(zip, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK ||
        unzOpenCurrentFile(zip) != UNZ_OK) {
        throw std::runtime_error("Cannot read JAR entry: " + name);
    }
    using CurrentEntry = std::unique_ptr<void, int (*)(void*)>;
    CurrentEntry entry(zip, [](void* handle) {
        return unzCloseCurrentFile(static_cast<unzFile>(handle));
    });
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.uncompressed_size));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto chunk = std::min<std::size_t>(bytes.size() - offset, 1u << 20);
        const auto count = unzReadCurrentFile(zip, bytes.data() + offset, static_cast<unsigned>(chunk));
        if (count <= 0) {
            throw std::runtime_error("Truncated JAR entry: " + name);
        }
        offset += static_cast<std::size_t>(count);
    }
    const int entry_status = unzCloseCurrentFile(zip);
    entry.release();
    if (entry_status != UNZ_OK) throw std::runtime_error("JAR CRC check failed: " + name);
    return bytes;
}

std::vector<std::uint8_t> read_zip_entry(const std::string& jar, const std::string& name) {
    return GameArchive(jar).read_entry(name);
}

GameMap load_map(const std::string& jar, int map_id) {
    return load_map(GameArchive(jar), map_id);
}

GameMap load_map(const GameArchive& archive, int map_id) {
    if (map_id < 0 || map_id > 99) throw std::out_of_range("map-id must be between 0 and 99");
    char path[32];
    std::snprintf(path, sizeof(path), "m/%02d.map", map_id);
    auto bytes = archive.read_entry(path);
    if (bytes.size() < 3) throw std::runtime_error("Map header is truncated");
    GameMap map{bytes[0], bytes[1], bytes[2], {}, {}};
    if (map.width == 0 || map.height == 0) throw std::runtime_error("Map dimensions must be nonzero");
    const auto cells = static_cast<std::size_t>(map.width) * map.height;
    if (bytes.size() != 3 + cells) throw std::runtime_error("Unexpected map file size");
    map.terrain.assign(bytes.begin() + 3, bytes.end());
    return map;
}

EventGrid parse_event_grid(const std::vector<std::uint8_t>& bytes, const GameMap& map) {
    const auto cells = static_cast<std::size_t>(map.width) * map.height;
    std::size_t pos = cells;
    auto take = [&](std::size_t count) {
        if (count > bytes.size() - std::min(pos, bytes.size()))
            throw std::runtime_error("Truncated event section");
        const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(pos);
        pos += count;
        return std::vector<std::uint8_t>(begin, begin + static_cast<std::ptrdiff_t>(count));
    };
    if (bytes.size() < cells) throw std::runtime_error("Event file is shorter than overlay grid");
    EventGrid event;
    event.overlay.assign(bytes.begin(), bytes.begin() + cells);
    auto fixed_table = [&](std::size_t stride) {
        const auto count = take(1)[0];
        return take(static_cast<std::size_t>(count) * stride);
    };
    event.object_resources = take(take(1)[0]);
    event.objects = fixed_table(5);
    event.npc_resources = take(take(1)[0]);
    event.npcs = fixed_table(3);
    event.enemy_resources = take(take(1)[0]);
    event.enemies = fixed_table(3);
    event.face_resources = take(take(1)[0]);
    const auto condition_lists = take(1)[0];
    for (std::size_t i = 0; i < condition_lists; ++i) {
        const auto count = take(1)[0];
        event.conditions.push_back(take(static_cast<std::size_t>(count) * 7));
    }
    const auto action_lists = take(1)[0];
    for (std::size_t i = 0; i < action_lists; ++i) {
        const auto count = take(1)[0];
        event.actions.push_back(take(static_cast<std::size_t>(count) * 3));
    }
    const auto string_count = take(1)[0];
    for (std::size_t i = 0; i < string_count; ++i)
        event.strings.push_back(take(take(1)[0]));
    const auto setup_condition_count = take(1)[0];
    event.setup_conditions = take(static_cast<std::size_t>(setup_condition_count) * 3);
    const auto setup_group_count = take(1)[0];
    for (std::size_t i = 0; i < setup_group_count; ++i) {
        const auto count = take(1)[0];
        event.setup_groups.push_back(take(static_cast<std::size_t>(count) * 4));
    }
    event.object_visible.assign(event.objects.size() / 5, true);
    event.npc_visible.assign(event.npcs.size() / 3, true);
    event.parsed_bytes = pos;
    event.trailing_bytes = bytes.size() - pos;
    return event;
}

EventGrid load_event_grid(const std::string& jar, const std::string& zone, int map_id, const GameMap& map) {
    return load_event_grid(GameArchive(jar), zone, map_id, map);
}

EventGrid load_event_grid(const GameArchive& archive, const std::string& zone, int map_id, const GameMap& map) {
    if (map_id < 0 || map_id > 99) throw std::out_of_range("map-id must be between 0 and 99");
    if (zone.empty() || zone.find('/') != std::string::npos || zone.find('\\') != std::string::npos ||
        zone == "." || zone == "..") throw std::invalid_argument("invalid event zone");
    char path[64];
    std::snprintf(path, sizeof(path), "m/%s/%02d.evt", zone.c_str(), map_id);
    auto bytes = archive.read_entry(path);
    return parse_event_grid(bytes, map);
}

std::vector<std::uint8_t> parse_switch_hex(const std::string& hex) {
    std::string digits = hex;
    if (digits.rfind("0x", 0) == 0 || digits.rfind("0X", 0) == 0) digits.erase(0, 2);
    if (digits.empty() || digits.size() % 2 != 0)
        throw std::invalid_argument("switch bitset must contain an even number of hex digits");
    if (digits.size() > 256)
        throw std::invalid_argument("switch bitset cannot exceed 128 bytes (1024 switch IDs)");
    std::vector<std::uint8_t> bytes;
    bytes.reserve(digits.size() / 2);
    for (std::size_t i = 0; i < digits.size(); i += 2) {
        std::size_t consumed = 0;
        const auto value = std::stoul(digits.substr(i, 2), &consumed, 16);
        if (consumed != 2) throw std::invalid_argument("invalid switch bitset hex");
        bytes.push_back(static_cast<std::uint8_t>(value));
    }
    return bytes;
}

std::vector<MapEntity> decode_objects(const EventGrid& event) {
    std::vector<MapEntity> result;
    result.reserve(event.objects.size() / 5);
    for (std::size_t i = 0; i < event.objects.size(); i += 5) {
        const auto frame = event.objects[i + 4];
        const auto dx = event.objects[i + 2] >= 128
            ? static_cast<int>(event.objects[i + 2]) - 256 : event.objects[i + 2];
        const auto dy = event.objects[i + 3] >= 128
            ? static_cast<int>(event.objects[i + 3]) - 256 : event.objects[i + 3];
        result.push_back({event.objects[i], event.objects[i + 1], 0, frame, -1, frame,
                          static_cast<std::int8_t>(dx), static_cast<std::int8_t>(dy)});
    }
    return result;
}

std::vector<MapEntity> decode_npcs(const EventGrid& event) {
    std::vector<MapEntity> result;
    result.reserve(event.npcs.size() / 3);
    for (std::size_t i = 0; i < event.npcs.size(); i += 3) {
        const auto raw_code = event.npcs[i + 2];
        const int code = raw_code >= 128 ? static_cast<int>(raw_code) - 256 : raw_code;
        MapEntity entity{event.npcs[i], event.npcs[i + 1], raw_code, 0, -1, -1, 0, 0};
        if (code >= 18) {
            entity.frame_index = code - 18;
        } else {
            int slot = 0;
            for (const auto raw_ref : event.npc_resources) {
                const int ref = raw_ref >= 128 ? static_cast<int>(raw_ref) - 256 : raw_ref;
                if (ref == code) {
                    entity.resource_slot = slot;
                    break;
                }
                if (ref < 18) ++slot;
            }
        }
        result.push_back(entity);
    }
    return result;
}

std::vector<MapEntity> decode_enemies(const EventGrid& event) {
    std::vector<MapEntity> result;
    result.reserve(event.enemies.size() / 3);
    for (std::size_t i = 0; i < event.enemies.size(); i += 3) {
        const auto raw_id = event.enemies[i + 2];
        const int id = raw_id >= 128 ? static_cast<int>(raw_id) - 256 : raw_id;
        int slot = -1;
        for (std::size_t j = 0; j < event.enemy_resources.size(); ++j) {
            const auto raw_ref = event.enemy_resources[j];
            const int ref = raw_ref >= 128 ? static_cast<int>(raw_ref) - 256 : raw_ref;
            if (ref == id) { slot = static_cast<int>(j); break; }
        }
        result.push_back({event.enemies[i], event.enemies[i + 1], raw_id, 0, slot, -1, 0, 0});
    }
    return result;
}

std::optional<std::pair<int, std::vector<std::uint8_t>>> select_setup_group(
    const EventGrid& event, const std::vector<std::uint8_t>& switch_bits) {
    const auto count = event.setup_conditions.size() / 3;
    for (std::size_t i = 0; i < count; ++i) {
        const auto hi = event.setup_conditions[i * 3];
        const auto lo = event.setup_conditions[i * 3 + 1];
        const auto raw_group = event.setup_conditions[i * 3 + 2];
        const int group = raw_group >= 128 ? static_cast<int>(raw_group) - 256 : raw_group;
        const std::size_t switch_id = (static_cast<std::size_t>(hi & 0x03) << 8) | lo;
        const std::size_t byte_index = switch_id / 8;
        const auto mask = static_cast<std::uint8_t>(1u << (switch_id % 8));
        if (byte_index >= switch_bits.size() || (switch_bits[byte_index] & mask) == 0) continue;
        if (group == -1) continue;
        if (group < -1) return std::nullopt;
        if (static_cast<std::size_t>(group) >= event.setup_groups.size())
            throw std::runtime_error("setup condition references invalid group");
        return std::make_pair(group, event.setup_groups[static_cast<std::size_t>(group)]);
    }
    return std::nullopt;
}

void apply_setup_group(GameMap& map, EventGrid& event, const std::vector<std::uint8_t>& commands) {
    if (commands.size() % 4 != 0) throw std::invalid_argument("setup command data is not 4-byte aligned");
    const auto cells = static_cast<std::size_t>(map.width) * map.height;
    for (std::size_t i = 0; i < commands.size(); i += 4) {
        const auto code = commands[i];
        const auto a = commands[i + 1];
        const auto b = commands[i + 2];
        const auto c = commands[i + 3];
        if (code == 100 || code == 101) {
            const std::size_t x = a, y = b, index = y * map.width + x;
            if (x >= map.width || y >= map.height || index >= cells || index >= event.overlay.size())
                throw std::out_of_range("setup tile coordinate outside map");
            if (code == 100) map.terrain[index] = c;
            else event.overlay[index] = c;
        } else if (code == 102 || code == 104) {
            const auto index = c;
            auto& records = code == 102 ? event.objects : event.npcs;
            const std::size_t stride = code == 102 ? 5 : 3;
            if (index >= records.size() / stride) throw std::out_of_range("setup entity index outside table");
            records[index * stride] = a;
            records[index * stride + 1] = b;
        } else if (code == 103 || code == 105) {
            const auto index = a;
            auto& visible = code == 103 ? event.object_visible : event.npc_visible;
            if (index >= visible.size()) throw std::out_of_range("setup hide index outside table");
            visible[index] = false;
        } else if (code == 108) {
            continue;
        } else if (code == 109) {
            if (a >= event.objects.size() / 5) throw std::out_of_range("setup object image index outside table");
            event.objects[static_cast<std::size_t>(a) * 5 + 4] = b;
        } else if (code == 110) {
            if (a >= event.npcs.size() / 3) throw std::out_of_range("setup NPC visual-code index outside table");
            event.npcs[static_cast<std::size_t>(a) * 3 + 2] = b;
        } else if (code == 111) {
            // ae.a: clear hero effects, then aw(10,-1,a-1) when a != 0.
            event.hero_emote = a;
        } else if (code == 112) {
            // Same permanent emote queue for NPC a, with icon b-1.
            if (a >= event.npcs.size() / 3) throw std::out_of_range("setup emote references missing NPC");
            event.npc_emotes.resize(event.npcs.size() / 3);
            event.npc_emotes[a] = b;
        } else if (code == 106 || code == 107) {
            throw std::runtime_error("setup opcode invokes ASSERT FAILED in original game");
        } else {
            throw std::runtime_error("unsupported setup opcode " + std::to_string(code));
        }
    }
}
