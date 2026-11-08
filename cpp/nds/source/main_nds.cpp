// Nintendo DS front end (docs/build/nds.md): the game JAR from the SD card, the field on the
// main 2D engine (terrain and objects in a wrapping 8 bpp bitmap BG, hero/NPCs/enemies as 16 bpp
// bitmap sprites) and the event scripts of the ported core with dialogue and choices on the bottom
// screen. The game flow follows main.cpp's logic tick; combat, menus, shops and audio are not here yet.
#include <nds.h>
#include <fat.h>
#include <sys/stat.h>
#include <malloc.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "bitmap_font.hpp"
#include "character_sprite.hpp"
#include "combat.hpp"
#include "consumable.hpp"
#include "save_game.hpp"
#include "title_menu.hpp"
#include "inventory_rules.hpp"
#include "item_catalog.hpp"
#include "eif.hpp"
#include "equipment.hpp"
#include <functional>
#include "event_runtime.hpp"
#include "event_trigger.hpp"
#include "event_visual.hpp"
#include "guardian.hpp"
#include "hero_movement.hpp"
#include "hero_progress.hpp"
#include "quest_journal.hpp"
#include "port_settings.hpp"
#include "service.hpp"
#include "ending.hpp"
#include "recipe_journal.hpp"
#include "shop_transaction.hpp"
#include "hero_vitals.hpp"
#include "language.hpp"
#include "language_pack.hpp"
#include "map.hpp"
#include "new_game.hpp"
#include "npc_movement.hpp"
#include "npc_sprite.hpp"
#include "sprite.hpp"
#include "nds_sound.hpp"

extern "C" const std::uint32_t game_jar_size __attribute__((weak)); // heroes_lore_jar.nds: the linked JAR (find_jar)

namespace {

constexpr int kRingW = 512, kRingH = 256;
constexpr int kScreenW = 256, kScreenH = 192;
constexpr int kCanvas = 64, kAnchorX = 32, kAnchorY = 44; // sprite canvas: anchor = entity centre
constexpr int kSpriteSlots = 12;                          // 12 x 8 KB 64x64 bitmap sprites in VRAM B (+16 x 2 KB 32x32)

// ---------------------------------------------------------------------------------------------
// Diagnostics: heap, 60 Hz clock, log file next to the JAR.

struct Memory {
    std::size_t used{}, peak{}, free{}, total{};
} memory;

void sample_memory() {
    const struct mallinfo info = mallinfo();
    memory.used = std::size_t(info.uordblks);
    memory.peak = std::max(memory.peak, memory.used);
    memory.free = std::size_t(getHeapLimit() - getHeapEnd()) + std::size_t(info.arena - info.uordblks);
    memory.total = std::size_t(getHeapLimit() - getHeapStart());
}

volatile u32 vblanks = 0;
void on_vblank() { ++vblanks; }

std::string log_path; // opened per line so a power-off keeps it
void log_file(const std::string& text) {
    if (log_path.empty()) return;
    if (FILE* file = std::fopen(log_path.c_str(), "a")) {
        std::fprintf(file, "[%lu] %s\n", static_cast<unsigned long>(vblanks * 1000 / 60), text.c_str());
        std::fclose(file);
    }
}

// ---------------------------------------------------------------------------------------------
// Decoded frames.

u16 to_rgb15(const std::uint8_t* p) { return u16(RGB15(p[0] >> 3, p[1] >> 3, p[2] >> 3)); }

// One BG palette per map: index 0 stays transparent (backdrop).
struct Palette {
    std::vector<u16> colors;
    std::uint8_t index(u16 color) {
        for (std::size_t i = 0; i < colors.size(); ++i) if (colors[i] == color) return std::uint8_t(i + 1);
        if (colors.size() < 255) { colors.push_back(color); return std::uint8_t(colors.size()); }
        int best = 0, best_d = 1 << 30; // full: nearest existing colour
        for (std::size_t i = 0; i < colors.size(); ++i) {
            const int dr = int(colors[i] & 31) - int(color & 31), dg = int((colors[i] >> 5) & 31) - int((color >> 5) & 31),
                      db = int((colors[i] >> 10) & 31) - int((color >> 10) & 31);
            const int d = dr * dr + dg * dg + db * db;
            if (d < best_d) { best_d = d; best = int(i); }
        }
        return std::uint8_t(best + 1);
    }
    void upload() const {
        BG_PALETTE[0] = RGB15(0, 0, 0);
        for (std::size_t i = 0; i < colors.size(); ++i) BG_PALETTE[i + 1] = colors[i];
    }
};

struct IndexedFrame { int w{}, h{}; std::vector<std::uint8_t> px; }; // 0 = transparent
struct ColorFrame { int w{}, h{}; std::vector<u16> px; };            // bit 15 = opaque

// Hot pixel loops: ARM code in ITCM, clipped once per call instead of per pixel.
#define HOT ITCM_CODE __attribute__((target("arm"), optimize("O2"), noinline))
// Non-zero pixels of a w x h frame at (x, y) into dst (stride, clipped to dw x dh).
HOT void blit16(u16* dst, int stride, int dw, int dh, const u16* src, int w, int h, int x, int y, bool flip) {
    const int y0 = y < 0 ? -y : 0, y1 = h < dh - y ? h : dh - y, x0 = x < 0 ? -x : 0, x1 = w < dw - x ? w : dw - x;
    if (x0 >= x1 || y0 >= y1) return;
    for (int yy = y0; yy < y1; ++yy) {
        u16* d = dst + (y + yy) * stride + x;
        const u16* row = src + yy * w;
        if (!flip) { for (int xx = x0; xx < x1; ++xx) if (const u16 v = row[xx]) d[xx] = v; }
        else { const u16* r = row + w - 1; for (int xx = x0; xx < x1; ++xx) if (const u16 v = r[-xx]) d[xx] = v; }
    }
}
// Indexed frame (0 transparent, i -> pal[i - 1], n colours) into a 16 bpp buffer.
HOT void blit_pal(u16* dst, int stride, int dw, int dh, const std::uint8_t* src, int w, int h, int x, int y, const u16* pal, int n) {
    const int y0 = y < 0 ? -y : 0, y1 = h < dh - y ? h : dh - y, x0 = x < 0 ? -x : 0, x1 = w < dw - x ? w : dw - x;
    if (x0 >= x1 || y0 >= y1) return;
    for (int yy = y0; yy < y1; ++yy) {
        u16* d = dst + (y + yy) * stride + x;
        const std::uint8_t* row = src + yy * w;
        for (int xx = x0; xx < x1; ++xx) { const int i = row[xx]; if (i && i <= n) d[xx] = pal[i - 1]; }
    }
}
// Word copy in 32-byte bursts (ldm/stm: one main-RAM burst per 8 words), the rest word by word.
HOT void copy32(u32* dst, const u32* src, int words) {
    int blocks = words >> 3;
    if (blocks > 0)
        asm volatile("1: ldmia %1!, {r3-r10}\n\tstmia %0!, {r3-r10}\n\tsubs %2, %2, #1\n\tbne 1b"
                     : "+r"(dst), "+r"(src), "+r"(blocks) : : "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "memory", "cc");
    for (int i = 0; i < (words & 7); ++i) dst[i] = src[i];
}
HOT void fill32(u32* dst, u32 v, int words) { for (int i = 0; i < words; ++i) dst[i] = v; }

std::unordered_map<std::size_t, IndexedFrame> load_indexed(const GameArchive& archive, const std::string& stem,
                                                           const std::unordered_set<std::size_t>& ids, Palette& palette) {
    std::unordered_map<std::size_t, IndexedFrame> out;
    if (ids.empty()) return out;
    const std::vector<std::size_t> list(ids.begin(), ids.end());
    for (auto& entry : load_sprite_frames_png(archive, stem, list)) {
        const auto image = decode_indexed_png(entry.second);
        IndexedFrame frame{int(image.width), int(image.height), {}};
        frame.px.resize(std::size_t(frame.w) * std::size_t(frame.h));
        for (std::size_t i = 0; i < frame.px.size(); ++i) {
            const auto* p = &image.rgba[i * 4];
            frame.px[i] = p[3] < 128 ? 0 : palette.index(to_rgb15(p));
        }
        out.emplace(entry.first, std::move(frame));
    }
    return out;
}

// Frames of one atlas as 16 bpp sprite pixels, stored under key_base + frame id.
std::size_t load_color_frames(const GameArchive& archive, const std::string& stem, const std::vector<std::size_t>& ids,
                              int key_base, std::unordered_map<int, ColorFrame>& out) {
    std::vector<std::size_t> missing;
    for (const auto id : ids) if (!out.count(key_base + int(id))) missing.push_back(id);
    if (missing.empty()) return 0;
    std::size_t bytes = 0;
    for (auto& entry : load_sprite_frames_png(archive, stem, missing)) {
        const auto image = decode_indexed_png(entry.second);
        ColorFrame frame{int(image.width), int(image.height), {}};
        frame.px.resize(std::size_t(frame.w) * std::size_t(frame.h));
        for (std::size_t i = 0; i < frame.px.size(); ++i) {
            const auto* p = &image.rgba[i * 4];
            frame.px[i] = p[3] < 128 ? 0 : u16(to_rgb15(p) | 0x8000);
        }
        bytes += frame.px.size() * 2;
        out.emplace(key_base + int(entry.first), std::move(frame));
    }
    return bytes;
}

// ---------------------------------------------------------------------------------------------
// Game state.

struct Persistent { // main.cpp's persistent_* values
    int character_class{6};
    std::string zone{"6"};
    std::vector<std::uint8_t> switches, quests;
    std::vector<InventoryStack> inventory;
    std::int64_t money{300};
    HeroVitals vitals;
    GuardianRoster guardians;
    HeroProgress progress;
    EquipmentSet equipment;
    std::vector<std::uint8_t> recipes; // port: recipe journal slots (kept for saves from other ports)
    int new_game_plus{}, clear_progress{}; // n.g Hardcore runs after an ending, ao.f banked progress
} game;


struct World {
    int map_id{-1};
    GameMap map;
    EventGrid event;
    std::vector<MapEntity> objects, npcs, enemies;
    std::vector<NpcMotionState> npc_motion;
    std::unique_ptr<HeroInput> hero;
    EventVisualState visuals;
    std::optional<std::size_t> last_auto_cell;
    bool peaceful{};
    int load_ms{}, colors{}, terrain_frames{}, object_frames{};
} world;

std::vector<std::string> text_bank;

struct EventState {
    std::unique_ptr<EventRuntime> runtime;
    std::vector<std::uint8_t> actions;
    bool dialogue{}, choice{}, choice_yes{true}, advance{}, item_message{};
    int portrait{}; // m/face frame + 1; negative: on the right
    std::optional<bool> choice_response;
    std::string text;
    int page{};
    std::string status; // unsupported command: shown until B ends the event
} ev;

bool ui_dirty = true;
namespace ui { // Modern menus (defined after the shop)
bool modern_on();
void draw_menu_modern();
void draw_shop_modern();
void menu_overlays(int tab, std::vector<std::pair<const char*, std::string>>& keys);
}
int text_language = 0; // 1: the Russian pack is loaded
std::string tr(const char* en, const char* ru) { return text_language == 1 ? ru : en; }
bool screen_open();   // shop or service on the bottom screen
bool shop_is_active();
void open_shop(const GameArchive& archive);
void open_service(EventScreen screen);
void start_ending(const GameArchive& archive);
bool ending_running();
bool ui_row_dirty = false; // HP/SP/EXP row or the enemy bar changed
bool guardian_buttons_dirty = false; // a guardian skill's cooldown cover changed
bool arena_dirty = false;  // boss arena: redraw the bottom half of the field (rows 0..130)
bool autosave_pending = false; // port: autosave.hls once a map entry has settled
// In-game menu on START / MENU (until the full game menu is on DS): Continue, Save, Title.
bool game_menu = false, game_menu_confirm = false;
bool menu_requested = false; // START while the hero is mid-step: the menu opens once he stands
int game_menu_selection = 0;
bool request_title = false;
int arena_sx = 0, arena_oy = 0; // bottom-screen arena viewport (map pixels), set each frame

// ---------------------------------------------------------------------------------------------
// Field background: maps reach 50x40 cells (800x640 px), more than any DS background. The 8 bpp
// bitmap BG of 512x256 wraps around and holds a 32x16-cell window; cells entering the view (plus
// one cell of margin) are queued and drawn a few per frame.

int bg_main = -1;
constexpr int kRingCellsX = kRingW / 16, kRingCellsY = kRingH / 16;
constexpr int kNoOrigin = -100000;
constexpr int kCellsPerFrame = 12;

struct PlacedObject { int left{}, top{}; const IndexedFrame* frame{}; int depth{}; }; // depth: aq.c y*16+offset_y
struct MapArt {
    Palette palette;
    std::unordered_map<std::size_t, IndexedFrame> terrain, objects;
    std::array<const IndexedFrame*, 256> terrain_by_id{};
    std::vector<PlacedObject> placed;
    std::vector<std::vector<std::uint16_t>> cell_objects;
    std::vector<std::uint8_t> cells;
    int width{}, height{};
    int origin_x{kNoOrigin}, origin_y{kNoOrigin};
    std::vector<std::pair<int, int>> pending;
    std::size_t pending_head{};
    int cells_drawn{};
} map_art;

std::vector<std::uint8_t> colour_index;  // RGB15 -> BG palette index (0 = not yet assigned)

int floor_div16(int v) { return v >= 0 ? v / 16 : -((15 - v) / 16); }

void render_cell(int cx, int cy) {
    alignas(4) std::uint8_t cell[16 * 16];
    std::memset(cell, 0, sizeof(cell));
    if (cx >= 0 && cy >= 0 && cx < map_art.width && cy < map_art.height) {
        const std::size_t index = std::size_t(cy) * map_art.width + cx;
        if (const IndexedFrame* f = map_art.terrain_by_id[map_art.cells[index]]) {
            if (f->w == 16 && f->h >= 16) std::memcpy(cell, f->px.data(), sizeof(cell));
            else
                for (int y = 0; y < std::min(16, f->h); ++y)
                    std::memcpy(cell + y * 16, f->px.data() + std::size_t(y) * f->w, std::size_t(std::min(16, f->w)));
        }
        const int left = cx * 16, top = cy * 16;
        for (const auto object : map_art.cell_objects[index]) {
            const auto& o = map_art.placed[object];
            const auto& f = *o.frame;
            const int x0 = std::max(left, o.left), x1 = std::min(left + 16, o.left + f.w);
            const int y0 = std::max(top, o.top), y1 = std::min(top + 16, o.top + f.h);
            for (int y = y0; y < y1; ++y) {
                const std::uint8_t* src = f.px.data() + std::size_t(y - o.top) * f.w + (x0 - o.left);
                std::uint8_t* dst = cell + (y - top) * 16 + (x0 - left);
                for (int x = x0; x < x1; ++x, ++src, ++dst) if (*src) *dst = *src;
            }
        }
    }
    u32* dst = reinterpret_cast<u32*>(bgGetGfxPtr(bg_main)) +
               (((cy & (kRingCellsY - 1)) * 16) * kRingW + (cx & (kRingCellsX - 1)) * 16) / 4;
    const u32* src = reinterpret_cast<const u32*>(cell);
    for (int y = 0; y < 16; ++y, dst += kRingW / 4, src += 4) { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = src[3]; }
    ++map_art.cells_drawn;
}

bool cell_in_window(int cx, int cy) {
    return map_art.origin_x != kNoOrigin && cx >= map_art.origin_x && cx < map_art.origin_x + kRingCellsX &&
           cy >= map_art.origin_y && cy < map_art.origin_y + kRingCellsY;
}

void update_ring(int sx, int sy) {
    const int vx0 = floor_div16(sx) - 1, vx1 = floor_div16(sx + kScreenW - 1) + 1;
    const int vy0 = floor_div16(sy) - 1, vy1 = floor_div16(sy + kScreenH - 1) + 1;
    const int old_x = map_art.origin_x, old_y = map_art.origin_y;
    int ox = old_x, oy = old_y;
    if (ox == kNoOrigin) { ox = vx0 - (kRingCellsX - (vx1 - vx0 + 1)) / 2; oy = vy0 - (kRingCellsY - (vy1 - vy0 + 1)) / 2; }
    if (vx0 < ox) ox = vx0;
    if (vx1 >= ox + kRingCellsX) ox = vx1 - kRingCellsX + 1;
    if (vy0 < oy) oy = vy0;
    if (vy1 >= oy + kRingCellsY) oy = vy1 - kRingCellsY + 1;
    if (ox != old_x || oy != old_y) {
        map_art.origin_x = ox;
        map_art.origin_y = oy;
        for (int cy = oy; cy < oy + kRingCellsY; ++cy)
            for (int cx = ox; cx < ox + kRingCellsX; ++cx) {
                if (old_x != kNoOrigin && cx >= old_x && cx < old_x + kRingCellsX && cy >= old_y && cy < old_y + kRingCellsY) continue;
                if (old_x == kNoOrigin) render_cell(cx, cy);
                else map_art.pending.push_back({cx, cy});
            }
    }
    // Cells already on screen are drawn now (a fast camera pan showed stale bands); the margin a few
    // per frame. A queued cell that left the window would overwrite another cell's slot.
    if (map_art.pending_head >= map_art.pending.size()) return;
    const int sx0 = floor_div16(sx), sx1 = floor_div16(sx + kScreenW - 1), sy0 = floor_div16(sy), sy1 = floor_div16(sy + kScreenH - 1);
    int budget = kCellsPerFrame;
    std::size_t keep = 0;
    for (std::size_t i = map_art.pending_head; i < map_art.pending.size(); ++i) {
        const auto cell = map_art.pending[i];
        if (!cell_in_window(cell.first, cell.second)) continue;
        const bool on_screen = cell.first >= sx0 && cell.first <= sx1 && cell.second >= sy0 && cell.second <= sy1;
        if (on_screen || budget > 0) { render_cell(cell.first, cell.second); if (!on_screen) --budget; }
        else map_art.pending[keep++] = cell;
    }
    map_art.pending.resize(keep);
    map_art.pending_head = 0;
}

bool arena_bg_valid = false; // boss arena background cache (terrain + objects, 16 bpp)
std::vector<u16> arena_bg;
// Boss frames as opaque runs per row (x, length), with the 8 bpp page indices; built on first use,
// dropped with the map (the palette and the frames change then).
struct FrameRuns { std::vector<u16> row_start, runs; std::vector<std::uint8_t> idx; bool has_idx = false; };
std::unordered_map<const void*, FrameRuns> boss_runs;
void redraw_ring() { arena_bg_valid = false; map_art.origin_x = map_art.origin_y = kNoOrigin; map_art.pending.clear(); map_art.pending_head = 0; }

std::string atlas_stem(char kind) {
    char stem[16];
    std::snprintf(stem, sizeof(stem), "m/t/%c%02u", kind, unsigned(world.map.kind));
    return stem;
}

void ensure_terrain_frames(const GameArchive& archive, const std::unordered_set<std::size_t>& ids) {
    std::unordered_set<std::size_t> missing;
    for (const auto id : ids) if (!map_art.terrain.count(id)) missing.insert(id);
    for (auto& entry : load_indexed(archive, atlas_stem('t'), missing, map_art.palette)) map_art.terrain.emplace(entry.first, std::move(entry.second));
    for (const auto& entry : map_art.terrain)
        if (entry.first < map_art.terrain_by_id.size()) map_art.terrain_by_id[entry.first] = &entry.second;
    world.terrain_frames = int(map_art.terrain.size());
}

// Objects: bottom-centre anchor at the tile plus offset (as the SDL renderer), indexed per cell.
void rebuild_objects(const GameArchive& archive) {
    std::unordered_set<std::size_t> missing;
    for (std::size_t i = 0; i < world.objects.size(); ++i)
        if ((i >= world.event.object_visible.size() || world.event.object_visible[i]) && world.objects[i].frame_index >= 0 &&
            !map_art.objects.count(std::size_t(world.objects[i].frame_index)))
            missing.insert(std::size_t(world.objects[i].frame_index));
    for (auto& entry : load_indexed(archive, atlas_stem('o'), missing, map_art.palette)) map_art.objects.emplace(entry.first, std::move(entry.second));
    world.object_frames = int(map_art.objects.size());
    map_art.placed.clear();
    arena_bg_valid = false;
    map_art.cell_objects.assign(std::size_t(map_art.width) * map_art.height, {});
    for (std::size_t i = 0; i < world.objects.size(); ++i) {
        if (i < world.event.object_visible.size() && !world.event.object_visible[i]) continue;
        const auto& o = world.objects[i];
        const auto found = map_art.objects.find(std::size_t(o.frame_index));
        if (o.frame_index < 0 || found == map_art.objects.end()) continue;
        const auto& f = found->second;
        const PlacedObject placed{o.x * 16 + o.offset_x - f.w / 2, o.y * 16 + o.offset_y - f.h, &f, o.y * 16 + o.offset_y};
        const auto id = std::uint16_t(map_art.placed.size());
        map_art.placed.push_back(placed);
        const int cx0 = std::max(0, floor_div16(placed.left)), cx1 = std::min(map_art.width - 1, floor_div16(placed.left + f.w - 1));
        const int cy0 = std::max(0, floor_div16(placed.top)), cy1 = std::min(map_art.height - 1, floor_div16(placed.top + f.h - 1));
        for (int cy = cy0; cy <= cy1; ++cy)
            for (int cx = cx0; cx <= cx1; ++cx) map_art.cell_objects[std::size_t(cy) * map_art.width + cx].push_back(id);
    }
}

void prepare_map(const GameArchive& archive) {
    map_art = MapArt{};
    colour_index.clear();
    boss_runs.clear();
    arena_bg_valid = false;
    std::vector<u16>().swap(arena_bg);
    map_art.width = world.map.width;
    map_art.height = world.map.height;
    map_art.cells = world.map.terrain;
    ensure_terrain_frames(archive, std::unordered_set<std::size_t>(world.map.terrain.begin(), world.map.terrain.end()));
    rebuild_objects(archive);
    world.colors = int(map_art.palette.colors.size()); // uploaded by load_world after the loading screen
}

// ---------------------------------------------------------------------------------------------
// Characters: hero layers (character tables), NPC and enemy animations, all composed into 64x64
// bitmap sprites. Only entities near the screen hold one of the 16 sprite slots.

struct HeroArt {
    int character_class{};
    std::array<CharacterSpriteTable, 6> tables;
    std::array<bool, 6> mirror{};
    std::unordered_map<int, ColorFrame> frames;
    std::array<EifAnimation, 3> magic; // Kail: c3/s/ea2..ea4 over category 5
    std::size_t bytes{};
} hero_art;

struct CharacterArt {
    std::unordered_map<int, NpcSpriteAnimations> npc_animations, enemy_animations;
    std::unordered_map<int, EifAnimation> enemy_effects; // enm/atef/NN over the enemy atlas
    std::unordered_map<int, ColorFrame> frames; // npc (code<<8|f), static npc 0x10000+f, enemy 0x20000+(id<<8|f)
    std::size_t bytes{};
} character_art;

void load_hero_art(const GameArchive& archive, int character_class) {
    hero_art = HeroArt{};
    hero_art.character_class = character_class;
    const auto layers = equipment_visual_layers(character_class, game.equipment); // the worn pieces
    for (std::size_t category = 0; category < layers.size(); ++category) {
        if (!layers[category].enabled) continue;
        hero_art.tables[category] = load_character_sprite_table(archive, character_class, layers[category].category);
        char stem[24];
        std::snprintf(stem, sizeof(stem), "c%d/i/%s", character_class - 5, layers[category].atlas.c_str());
        hero_art.mirror[category] = load_sprite_atlas(archive, stem).mirror_on_load();
        std::vector<std::size_t> ids;
        std::unordered_set<std::size_t> seen;
        for (const auto& record : hero_art.tables[category])
            for (const auto& frame : record.frames)
                for (const auto& layer : frame)
                    if (layer.atlas_frame != 0xff && seen.insert(layer.atlas_frame).second) ids.push_back(layer.atlas_frame);
        if (category == 5 && character_class == 8) // ao.d: Kail's magic frames live in the effect atlas
            for (int m = 0; m < 3; ++m) {
                hero_art.magic[std::size_t(m)] = load_eif(archive, "c3/s/ea" + std::to_string(m + 2));
                for (const auto& frame : hero_art.magic[std::size_t(m)])
                    for (const auto& layer : frame)
                        if (seen.insert(layer.atlas_frame).second) ids.push_back(layer.atlas_frame);
            }
        hero_art.bytes += load_color_frames(archive, stem, ids, int(category << 8), hero_art.frames);
    }
}

std::unordered_map<int, ColorFrame> ui_art; // bottom-screen art: faces 0x100+i, item icons 0x200+i, guardians 0x500+i
std::unordered_map<int, ColorFrame> menu_art; // game menu: sgui/gmico 0x700+i, img/glb 0x800+n (9..13, 15 from fr-FR PNGs)
std::unordered_map<int, ColorFrame> combat_art; // img/etcui 1, 3, 5..8; img/glb 7 (coin) as 0x107

std::vector<std::size_t> animation_frames(const NpcSpriteAnimations& animations) {
    std::unordered_set<std::size_t> seen;
    for (const auto* set : {&animations.idle, &animations.walk, &animations.attack})
        for (const auto& direction : *set)
            for (const auto& frame : direction)
                for (const auto& layer : frame) seen.insert(layer.frame);
    return {seen.begin(), seen.end()};
}

// Field combat state (main.cpp combat_world and friends).
combat::CombatWorld combat_world;
std::vector<combat::EnemyStats> enemy_stats;
std::vector<ShopItem> item_catalog;
std::vector<MapEntity> enemy_records; // the EVT's enemy list; world.enemies mirrors the live enemies
int deferred_attack = 0;              // 1 attack, 2 skill: queued until the hero stands on a tile
int view_x = 0, view_y = 0;           // last scroll, for the combat's visible area

// main.cpp load_npc_textures / load_enemy_textures, as 16 bpp frames.
void load_character_art(const GameArchive& archive) {
    character_art = CharacterArt{};
    std::vector<std::size_t> static_frames;
    for (const auto& npc : world.npcs) {
        if (npc.frame_index >= 0) { static_frames.push_back(std::size_t(npc.frame_index)); continue; }
        const int code = npc.code >= 128 ? int(npc.code) - 256 : npc.code;
        if (code < 0 || code >= 18 || character_art.npc_animations.count(code)) continue;
        auto animations = code == 3 || code == 6 ? load_enemy_animations(archive, code == 3 ? 17 : 20) : load_npc_animations(archive, code);
        char stem[16];
        std::snprintf(stem, sizeof(stem), "%s/%02d", code == 3 || code == 6 ? "enm" : "npc", code == 3 ? 17 : code == 6 ? 20 : code);
        character_art.bytes += load_color_frames(archive, stem, animation_frames(animations), code << 8, character_art.frames);
        character_art.npc_animations.emplace(code, std::move(animations));
    }
    character_art.bytes += load_color_frames(archive, "npc/all", static_frames, 0x10000, character_art.frames);
    for (const auto& enemy : enemy_records) {
        if (enemy.resource_slot < 0) continue;
        const int id = enemy.code >= 128 ? int(enemy.code) - 256 : enemy.code;
        if (id < 0 || id > 31 || character_art.enemy_animations.count(id)) continue;
        auto animations = load_enemy_animations(archive, id);
        char stem[16];
        std::snprintf(stem, sizeof(stem), "enm/%02d", id);
        auto ids = animation_frames(animations);
        char effect[24];
        std::snprintf(effect, sizeof(effect), "enm/atef/%02d", id);
        if (archive.contains(effect)) {
            auto eif = load_eif(archive, effect);
            for (const auto& frame : eif) for (const auto& layer : frame) if (layer.atlas_frame != 0xff) ids.push_back(layer.atlas_frame);
            character_art.enemy_effects.emplace(id, std::move(eif));
        }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        character_art.bytes += load_color_frames(archive, stem, ids, 0x20000 + (id << 8), character_art.frames);
        character_art.enemy_animations.emplace(id, std::move(animations));
    }
}

struct Layer { const ColorFrame* frame; int x, y; bool flip; int rows = 0; }; // rows > 0: only the top rows (sinking figures)
inline int layer_h(const Layer& l) { return l.rows > 0 && l.rows < l.frame->h ? l.rows : l.frame->h; }

void compose(u16* gfx, const std::vector<Layer>& layers, int size = kCanvas, int anchor_x = kAnchorX, int anchor_y = kAnchorY,
             int height = kCanvas, const std::vector<std::uint16_t>* occluders = nullptr, int map_left = 0, int map_top = 0) {
    static u16 canvas[kCanvas * kCanvas];
    std::memset(canvas, 0, std::size_t(size) * height * 2);
    for (const auto& layer : layers) {
        const auto& f = *layer.frame;
        blit16(canvas, size, size, height, f.px.data(), f.w, layer_h(layer), anchor_x + layer.x, anchor_y + layer.y, layer.flip);
    }
    // Map objects in front of the sprite (aq.c depth) are in the field bitmap below it: cut their
    // opaque pixels out of the sprite so the object shows through, as if drawn over it.
    if (occluders)
        for (const auto id : *occluders) {
            const auto& o = map_art.placed[id];
            const auto& f = *o.frame;
            const int x0 = std::max(map_left, o.left), x1 = std::min(map_left + size, o.left + f.w);
            const int y0 = std::max(map_top, o.top), y1 = std::min(map_top + height, o.top + f.h);
            for (int y = y0; y < y1; ++y) {
                const std::uint8_t* src = f.px.data() + std::size_t(y - o.top) * f.w + (x0 - o.left);
                u16* dst = canvas + (y - map_top) * size + (x0 - map_left);
                for (int x = x0; x < x1; ++x, ++src, ++dst) if (*src) *dst = 0;
            }
        }
    DC_FlushRange(canvas, std::size_t(size) * height * 2);
    dmaCopy(canvas, gfx, std::size_t(size) * height * 2);
}

// Same layer order as draw_hero_sprite in main.cpp: slots 0..8, categories inside. While attacking
// or dying the combat picks the sprite state (ao.e(): 3..10 attacks, 2 death facing up) and frame.
std::vector<Layer> hero_layers(const HeroState& hero) {
    std::vector<Layer> out;
    if (hero.direction < 1 || hero.direction > 4) return out;
    const bool fighting = combat_world.hero_attacking() || combat_world.hero_dying();
    const int combat_state = fighting ? combat_world.hero_sprite_state() : -1;
    const auto state = std::uint8_t(combat_state >= 0 ? combat_state : hero.motion_mode == 2 ? 1 : 0);
    const auto direction = std::uint8_t(combat_state == 2 ? 0 : hero.direction - 1);
    const int tick = combat_state >= 0 ? combat_world.hero_frame() : hero.movement_marker < 0 ? 0 : hero.movement_marker;
    for (std::uint8_t slot = 0; slot < 9; ++slot) {
        if (world.peaceful && (slot == 6 || slot == 7)) continue; // ao.a: weapon layers need ae.c
        for (std::size_t category = 0; category < hero_art.tables.size(); ++category) {
            const auto* record = find_character_sprite_record(hero_art.tables[category], slot, state, direction);
            if (!record || record->frames.empty()) continue;
            for (const auto& layer : record->frames[std::size_t(tick) % record->frames.size()]) {
                if (layer.atlas_frame == 0xff) continue;
                const auto found = hero_art.frames.find(int(category << 8) | layer.atlas_frame);
                if (found != hero_art.frames.end())
                    out.push_back({&found->second, layer.x, layer.y, layer.mirrored && hero_art.mirror[category]});
            }
        }
    }
    return out;
}

const std::vector<NpcSpriteLayer>* npc_sequence_frame(const NpcSpriteAnimations& animations, int direction, bool walking, std::size_t frame) {
    if (direction < 1 || direction > 4) direction = 2;
    const auto slot = std::size_t(direction - 1);
    const auto& sequence = walking && !animations.walk[slot].empty() ? animations.walk[slot] : animations.idle[slot];
    if (sequence.empty()) return nullptr;
    return &sequence[frame % sequence.size()];
}

// main.cpp live_enemy_frame (ordinary enemies): walk, attack, otherwise idle.
const std::vector<NpcSpriteLayer>* live_enemy_frame(const combat::Enemy& enemy) {
    const auto found = character_art.enemy_animations.find(enemy.sprite);
    if (found == character_art.enemy_animations.end() || enemy.direction < 1 || enemy.direction > 4) return nullptr;
    const auto& set = enemy.state == combat::EnemyState::walk ? found->second.walk
                      : enemy.state == combat::EnemyState::attack ? found->second.attack : found->second.idle;
    const auto& frames = set[std::size_t(enemy.direction - 1)];
    if (frames.empty()) return nullptr;
    return &frames[std::size_t(std::max(0, enemy.frame)) % frames.size()];
}

struct Entity { int owner, ax, ay; std::uint32_t key; int depth{INT_MIN}; }; // depth: aq.c sort y (ay when unset)
struct SpriteSlot { u16* gfx{}; int owner{-1}; std::uint32_t key{}; bool used{}; };
std::array<SpriteSlot, kSpriteSlots> slots;

std::uint32_t mix_key(std::uint32_t a, std::uint32_t b) { return (a * 2654435761u) ^ (b + 0x9e3779b9u + (a << 6) + (a >> 2)); }

// Combat effects (main.cpp draw_battle_effect / draw_status / draw_emote / draw_projectile).
// Effects attached to an actor (aw: hit flashes, LEVEL UP, atef bursts, emote bubbles, status
// icons) are composed into that actor's sprite; world-bound ones (death bursts, projectiles,
// y effects on a tile) take sprites of their own from the same pool.
int boss_texture_base_of(int stats_id) { return stats_id >= 42 ? 42 : stats_id >= 39 ? 39 : stats_id >= 35 ? 35 : 32; }
const GameArchive* current_archive = nullptr;
std::array<EifAnimation, 3> death_eif; // enm/die/0..2, atlas enm/die/bang (0x200+f)
EifAnimation lvup_eif;                  // char/lvup.eif, atlas char/lvup (0x400+f); caption 0x3ff
// Guardians (p, ce.d/ce.f): figures grd/0..5 frames 0, 1; element atlases grd/fi, grd/wa, grd/gr (by id % 3);
// skill EIFs grd/spr/{id}_{n}.eif (id 0 slot 0: 0_01), and 0_02 for effect kind 2.
constexpr int kGuardianFigure = 0x10000, kGuardianAtlas = 0x11000;
struct GuardianFx { std::array<EifAnimation, 3> slots; EifAnimation extra; };
std::array<GuardianFx, 6> guardian_fx;
std::vector<std::string> guardian_skill_names; // grd/grdsk.tdf: id * 8 + skill * 2

const ColorFrame* art_frame(const std::unordered_map<int, ColorFrame>& map, int key) {
    const auto found = map.find(key);
    return found == map.end() ? nullptr : &found->second;
}

// aw kinds relative to the owner's anchor (x+8, y+8).
void battle_effect_layers(const combat::BattleEffect& effect, std::vector<Layer>& out) {
    const auto piece = [&](int key, int dx, int dy, bool flip = false, bool centre = false, bool bottom = false) {
        if (const auto* f = art_frame(combat_art, key)) out.push_back({f, dx - (centre ? f->w / 2 : 0), dy - (bottom ? f->h : 0), flip});
    };
    if (effect.kind == 3) { // aw3: LEVEL UP caption HCENTER|BOTTOM at y+5, hidden every fourth frame
        if (effect.frame % 4 < 3) piece(0x3ff, 0, 5, false, true, true);
        return;
    }
    if (effect.kind == 4) { // aw4: lvup.eif frame
        if (std::size_t(effect.frame) < lvup_eif.size())
            for (const auto& layer : lvup_eif[std::size_t(effect.frame)])
                if (layer.atlas_frame != 0xff) piece(0x400 + layer.atlas_frame, layer.x, layer.y);
        return;
    }
    if (effect.kind >= combat::kEifEffectBase) { // aw9 / y: the actor's atef EIF, TOP|LEFT
        const int sprite = effect.kind - combat::kEifEffectBase;
        const auto found = character_art.enemy_effects.find(sprite);
        if (found == character_art.enemy_effects.end() || std::size_t(effect.frame) >= found->second.size()) return;
        for (const auto& layer : found->second[std::size_t(effect.frame)])
            if (layer.atlas_frame != 0xff)
                if (const auto* f = art_frame(character_art.frames, 0x20000 + ((sprite >= 32 ? boss_texture_base_of(sprite) : sprite) << 8) + layer.atlas_frame))
                    out.push_back({f, layer.x, layer.y, false});
        return;
    }
    if (effect.kind >= 200) { // aw10: emote bubble, backing except icons 8/9
        const int icon = effect.kind - 200;
        if (icon != 8 && icon != 9) piece(0x300, 0, -40, false, true);
        piece(0x600 + icon, 0, -39 + effect.frame % 2, false, true);
        return;
    }
    if (effect.kind >= 100 && effect.kind <= 102) {
        const auto& frames = death_eif[std::size_t(effect.kind - 100)];
        if (std::size_t(effect.frame) < frames.size())
            for (const auto& layer : frames[std::size_t(effect.frame)]) piece(0x200 + layer.atlas_frame, layer.x, layer.y);
        return;
    }
    const int bank = effect.kind == 6 ? 2 : effect.kind == 5 ? 1 : 0; // aw1/5/6: img/atteff1..3
    int dx = 0, dy = 3;
    if (effect.kind == 1) { if (effect.frame == 0) { dx = -3; dy -= 10; } else if (effect.frame == 1) dy -= 8; }
    if (effect.kind == 5 && effect.frame == 2) dy -= 5;
    if (effect.kind == 6) { if (effect.frame == 1) dy -= 2; else if (effect.frame == 2) dy -= 6; }
    piece(0x100 + bank * 16 + effect.frame, dx, dy, false, true, true);
}

// o.c / cf: status icons in reverse insertion order, 12 px apart, over keepst 0 backing.
void status_layers(const std::vector<int>& order, const std::function<int(int)>& elapsed, int top, std::vector<Layer>& out) {
    int x = -6 * (int(order.size()) - 1);
    for (auto it = order.rbegin(); it != order.rend(); ++it, x += 12) {
        if (const auto* f = art_frame(combat_art, 0x300)) out.push_back({f, x - f->w / 2, top, false});
        if (const auto* f = art_frame(combat_art, 0x301 + *it)) out.push_back({f, x - f->w / 2, top + 1 + elapsed(*it) % 2, false});
    }
}

void emote_layers(std::uint8_t id, std::vector<Layer>& out) { // event emote bubble (EMO_HERO / EMO_NPC)
    if (!id) return;
    const int frame = id - 1;
    if (frame != 8 && frame != 9)
        if (const auto* f = art_frame(combat_art, 0x300)) out.push_back({f, -f->w / 2, -40, false});
    if (const auto* f = art_frame(combat_art, 0x600 + frame)) out.push_back({f, -f->w / 2, -39 + int(vblanks / 5 % 2), false});
}

// Element atlas image (ce.a[12][n]) BOTTOM|HCENTER at (x, y) from the anchor.
void guardian_atlas(int id, int frame, int x, int y, std::vector<Layer>& out) {
    if (const auto* f = art_frame(combat_art, kGuardianAtlas + (id % 3) * 256 + frame)) out.push_back({f, x - f->w / 2, y - f->h, false});
}
// as.b: one .eif frame, layers TOP|LEFT from (x, y), images from the guardian's element atlas.
void guardian_eif(int id, const EifAnimation& eif, int frame, int x, int y, std::vector<Layer>& out) {
    if (frame < 0 || std::size_t(frame) >= eif.size()) return;
    for (const auto& layer : eif[std::size_t(frame)])
        if (layer.atlas_frame != 0xff)
            if (const auto* f = art_frame(combat_art, kGuardianAtlas + (id % 3) * 256 + layer.atlas_frame)) out.push_back({f, x + layer.x, y + layer.y, false});
}
// p.a(Graphics): the summoned guardian at its tile anchor (x*16+8, y*16+8): summon circle pieces,
// rising/sinking figure (pushed down `sink` tenths and clipped at the anchor), idle bob.
std::vector<Layer> guardian_layers() {
    std::vector<Layer> out;
    const auto& g = combat_world.guardian();
    if (g.state == 0 || g.id < 0 || g.id > 5) return out;
    const auto figure = [&](int n, int sink, int dy) {
        const auto* f = art_frame(combat_art, kGuardianFigure + g.id * 2 + n);
        if (!f) return;
        const int visible = f->h - f->h * sink / 10;
        if (visible > 0) out.push_back({f, -f->w / 2, -visible + dy, false, visible});
    };
    const auto atlas = [&](int frame, int dy) { guardian_atlas(g.id, frame, 0, dy, out); };
    if (g.state == 1) {
        switch (g.anim) {
            case 0: atlas(2, 0); break;
            case 1: atlas(3, 0); break;
            case 2: atlas(3, 0); atlas(0, 3); break;
            case 3: atlas(3, 0); atlas(0, 3); atlas(1, 6); break;
            case 4: atlas(2, 0); atlas(1, 6); break;
            case 5: figure(0, 7, 0); break;
            case 6: figure(0, 5, 0); atlas(2, 0); break;
            case 7: figure(0, 3, 0); atlas(3, 0); break;
            case 8: figure(0, 2, 0); atlas(3, 0); break;
            case 9: figure(0, 0, 0); atlas(2, 0); break;
            default: break;
        }
    } else if (g.state == 2) {
        figure(1, 0, g.loops % 3 == 0 ? 1 : 0);
        if (g.anim == 1 || g.anim == 2) atlas(0, 3);
        if (g.anim == 2 || g.anim == 3) atlas(1, 6);
    } else {
        if (g.anim < 6) figure(1, 0, 0);
        switch (g.anim) {
            case 6: figure(0, 1, 0); atlas(3, 0); atlas(0, 3); atlas(1, 6); break;
            case 7: figure(0, 3, 0); atlas(3, 0); atlas(0, 3); break;
            case 8: figure(0, 5, 0); atlas(3, 0); break;
            case 9: figure(0, 7, 0); atlas(2, 0); break;
            default: break;
        }
    }
    return out;
}
// y: an effect spawned by a guardian skill, anchored at its tile pixel + (8, 9).
std::vector<Layer> guardian_effect_layers(const combat::WorldEffect& e) {
    std::vector<Layer> out;
    const int id = combat_world.guardian().id;
    if (id < 0 || id > 5) return out;
    const auto& fx = guardian_fx[std::size_t(id)];
    static constexpr int slot_of_kind[11]{-1, 0, -1, -1, 0, 0, 1, 0, 1, 1, -1};
    switch (e.kind) {
        case 2: guardian_eif(id, fx.extra, e.frame, 0, 0, out); break;
        case 4: case 5: case 7: {
            if (e.frame >= 3) { guardian_eif(id, fx.slots[0], e.frame - 3, 0, 0, out); break; }
            const auto* f = art_frame(combat_art, kGuardianAtlas + (id % 3) * 256 + (e.kind == 4 ? 8 : 11));
            if (!f) break;
            const int visible = f->h - f->h * (e.frame == 0 ? 7 : e.frame == 1 ? 5 : 3) / 10;
            out.push_back({f, -f->w / 2, -visible, false, visible});
            break;
        }
        case 10: // y.d: atlas 0 at the anchor, atlas 1 three pixels lower
            if (e.frame <= 1) guardian_atlas(id, 0, 0, 0, out);
            if (e.frame >= 1) guardian_atlas(id, 1, 0, 3, out);
            break;
        default:
            if (e.kind >= 0 && e.kind < 11 && slot_of_kind[e.kind] >= 0) guardian_eif(id, fx.slots[std::size_t(slot_of_kind[e.kind])], e.frame, 0, 0, out);
            break;
    }
    return out;
}
// bj: an aura on the hero or a marked enemy (dy: from the owner's sprite anchor to the al.draw anchor).
void aura_layers(const combat::Aura& a, int dy, std::vector<Layer>& out) {
    if (a.delay > 0 || a.guardian < 0 || a.guardian > 5) return;
    if (a.guardian == 0 && a.skill == 0) {
        static constexpr int frames[6]{7, 8, 7, 8, 9, 9};
        if (a.frame < 6) guardian_atlas(0, frames[a.frame], 0, dy - std::min(a.frame, 4), out);
    } else if (a.skill == 2 && a.guardian <= 1) {
        const int phase = a.frame % 4;
        if (phase == 1 || phase == 2) guardian_atlas(a.guardian, 0, 0, dy + 9, out);
        if (phase == 2 || phase == 3) guardian_atlas(a.guardian, 1, 0, dy + 12, out);
    } else guardian_eif(a.guardian, guardian_fx[std::size_t(a.guardian)].slots[std::size_t(std::clamp(a.skill, 0, 2))], a.frame, 0, dy, out);
}

std::vector<Layer> attached_layers(int target) {
    std::vector<Layer> out;
    {
        int dy = 0;
        if (target >= 0 && std::size_t(target) < combat_world.enemies().size()) dy = -combat_world.enemies()[std::size_t(target)].draw_offset_y;
        for (const auto& a : combat_world.auras()) if (a.target == target) aura_layers(a, dy, out);
    }
    for (const auto& effect : combat_world.battle_effects())
        if (effect.target == target) battle_effect_layers(effect, out);
    if (target == -1) {
        const auto& s = combat_world.hero_status();
        status_layers(s.order, [&](int id) { return id == 5 ? 140 - s.attack_up : id == 6 ? 160 - s.defense_up : s.poison_tick; },
                      -38, out);
        emote_layers(world.visuals.hero_emote, out);
    } else if (target >= 0 && std::size_t(target) < combat_world.enemies().size()) {
        const auto& e = combat_world.enemies()[std::size_t(target)];
        int size = 0;
        if (e.stats >= 0 && std::size_t(e.stats) < enemy_stats.size()) size = enemy_stats[std::size_t(e.stats)].size;
        if (!e.status_order.empty())
            status_layers(e.status_order, [&](int id) { return e.status_elapsed[std::size_t(id)]; }, -30 - size * 3, out);
    }
    return out;
}

std::uint32_t attached_key(int target) {
    std::uint32_t key = 0x51ed;
    for (const auto& a : combat_world.auras())
        if (a.target == target && a.delay <= 0) key = mix_key(key, std::uint32_t(a.guardian << 12 | a.skill << 8 | a.frame) + 0x77u);
    for (const auto& effect : combat_world.battle_effects())
        if (effect.target == target) key = mix_key(key, std::uint32_t(effect.kind << 8 | effect.frame));
    if (target == -1) {
        const auto& s = combat_world.hero_status();
        for (const int id : s.order) key = mix_key(key, std::uint32_t(id << 4 | ((id == 5 ? 140 - s.attack_up : id == 6 ? 160 - s.defense_up : s.poison_tick) & 1)));
        if (world.visuals.hero_emote) key = mix_key(key, std::uint32_t(world.visuals.hero_emote << 1 | (vblanks / 5 % 2)));
    } else if (target >= 0 && std::size_t(target) < combat_world.enemies().size()) {
        const auto& e = combat_world.enemies()[std::size_t(target)];
        for (const int id : e.status_order) key = mix_key(key, std::uint32_t(id << 4 | (e.status_elapsed[std::size_t(id)] & 1)));
    }
    return key;
}

// World-bound effects: owner 2000+i projectile, 3000+i battle effect with target -2.
std::vector<Layer> world_effect_layers(int owner) {
    std::vector<Layer> out;
    if (owner >= 3000) {
        const auto i = std::size_t(owner - 3000);
        if (i < combat_world.battle_effects().size()) battle_effect_layers(combat_world.battle_effects()[i], out);
        return out;
    }
    const auto i = std::size_t(owner - 2000);
    if (i >= combat_world.projectiles().size()) return out;
    const auto& p = combat_world.projectiles()[i];
    const int frame = std::max(0, p.frame - 1);
    if (p.from_hero) { // ao.d: Kail's magic over the e atlas (variant 1 = mirrored)
        if (p.effect < 0 || p.effect > 2) return out;
        const auto& magic = hero_art.magic[std::size_t(p.effect)];
        if (std::size_t(frame) >= magic.size()) return out;
        for (const auto& layer : magic[std::size_t(frame)])
            if (const auto* f = art_frame(hero_art.frames, (5 << 8) | layer.atlas_frame)) out.push_back({f, layer.x, layer.y, layer.variant != 0});
        return out;
    }
    const auto found = character_art.enemy_effects.find(p.effect);
    if (found == character_art.enemy_effects.end() || std::size_t(frame) >= found->second.size()) return out;
    for (const auto& layer : found->second[std::size_t(frame)])
        if (layer.atlas_frame != 0xff)
            if (const auto* f = art_frame(character_art.frames, 0x20000 + ((p.effect >= 32 ? boss_texture_base_of(p.effect) : p.effect) << 8) + layer.atlas_frame))
                out.push_back({f, layer.x, layer.y, false});
    return out;
}

std::vector<Layer> entity_layers(int owner) {
    std::vector<Layer> out;
    if (owner == 4000) return guardian_layers();
    if (owner > 4000) {
        const auto i = std::size_t(owner - 4001);
        return i < combat_world.effects().size() ? guardian_effect_layers(combat_world.effects()[i]) : out;
    }
    if (owner >= 2000) return world_effect_layers(owner);
    if (owner == 0) {
        out = hero_layers(world.hero->state());
        const auto extra = attached_layers(-1);
        out.insert(out.end(), extra.begin(), extra.end());
        return out;
    }
    if (owner < 1000) {
        const std::size_t i = std::size_t(owner - 1);
        const auto& npc = world.npcs[i];
        if (npc.frame_index >= 0) {
            const auto found = character_art.frames.find(0x10000 + npc.frame_index);
            if (found != character_art.frames.end()) out.push_back({&found->second, -found->second.w / 2, -found->second.h, false});
        } else {
            const int code = npc.code >= 128 ? int(npc.code) - 256 : npc.code;
            const auto animations = character_art.npc_animations.find(code);
            if (animations != character_art.npc_animations.end()) {
                const auto& m = world.npc_motion[i];
                if (const auto* frame = npc_sequence_frame(animations->second, m.direction, m.motion_mode == 2, m.animation_frame))
                    for (const auto& layer : *frame) {
                        const auto found = character_art.frames.find((code << 8) | layer.frame);
                        if (found != character_art.frames.end()) out.push_back({&found->second, layer.x, layer.y, layer.mirrored});
                    }
            }
        }
        if (i < world.visuals.npc_emotes.size()) emote_layers(world.visuals.npc_emotes[i], out);
        return out;
    }
    const std::size_t i = std::size_t(owner - 1000);
    if (i >= combat_world.enemies().size()) return out;
    const auto& enemy = combat_world.enemies()[i];
    if (const auto* frame = live_enemy_frame(enemy))
        for (const auto& layer : *frame) {
            const auto found = character_art.frames.find(0x20000 + (enemy.sprite << 8) + layer.frame);
            if (found != character_art.frames.end()) out.push_back({&found->second, layer.x, layer.y, layer.mirrored});
        }
    const auto extra = attached_layers(int(i));
    out.insert(out.end(), extra.begin(), extra.end());
    return out;
}

std::vector<Entity> visible_entities(int sx, int sy) {
    std::vector<Entity> out, effects;
    const auto near_screen = [&](int ax, int ay) {
        return ax - sx > -kAnchorX && ax - sx < kScreenW + kAnchorX && ay - sy > -(kCanvas - kAnchorY) && ay - sy < kScreenH + kAnchorY;
    };
    if (world.hero) {
        const auto h = world.hero->state();
        const auto hit = combat_world.hero_hit_offset();
        const bool fighting = combat_world.hero_attacking() || combat_world.hero_dying();
        std::uint32_t key = mix_key(mix_key(std::uint32_t(h.direction), std::uint32_t(h.motion_mode)),
                                    std::uint32_t(h.movement_marker + 1) | (world.peaceful ? 0x100u : 0u));
        if (fighting) key = mix_key(key, std::uint32_t(combat_world.hero_sprite_state() << 8 | combat_world.hero_frame()) + 0x10000u);
        out.push_back({0, h.x_pixels + 8 + hit.first, h.y_pixels + 8 + hit.second, mix_key(key, attached_key(-1)), h.y_pixels + 8});
    }
    for (std::size_t i = 0; i < world.npcs.size(); ++i) {
        if (i < world.event.npc_visible.size() && !world.event.npc_visible[i]) continue;
        const auto& npc = world.npcs[i];
        if (npc.x >= world.map.width || npc.y >= world.map.height) continue;
        const int ax = npc.x * 16 + 8 + (npc.partial_x ? 8 : 0), ay = npc.y * 16 + 8 + (npc.partial_y ? 8 : 0);
        if (!near_screen(ax, ay)) continue;
        const auto& m = world.npc_motion[i];
        std::uint32_t key = mix_key(mix_key(std::uint32_t(m.direction), std::uint32_t(m.motion_mode)), m.animation_frame);
        if (i < world.visuals.npc_emotes.size() && world.visuals.npc_emotes[i])
            key = mix_key(key, std::uint32_t(world.visuals.npc_emotes[i] << 1 | (vblanks / 5 % 2)));
        out.push_back({int(i) + 1, ax, ay, key});
    }
    const auto& live = combat_world.enemies();
    for (std::size_t i = 0; i < live.size(); ++i) {
        const auto& e = live[i];
        if (e.state == combat::EnemyState::dead || e.hidden || e.boss != combat::BossKind::none) continue;
        int ax = e.x_px + 8 + (e.width - 1) * 8, ay = e.y_px + 8 + e.draw_offset_y;
        if (e.shake == 3 || e.shake == 1) { // al: hit shake, 3 px towards the blow
            ax += combat::kDx[std::size_t(e.shake_direction)] * 3;
            ay += combat::kDy[std::size_t(e.shake_direction)] * 3;
        }
        if (!near_screen(ax, ay)) continue;
        out.push_back({1000 + int(i), ax, ay,
                       mix_key(mix_key(mix_key(std::uint32_t(e.state), std::uint32_t(e.direction)), std::uint32_t(e.frame) ^ std::uint32_t(e.generation << 12)),
                               attached_key(int(i))), e.y_px + 8});
    }
    const auto& effects_list = combat_world.battle_effects();
    for (std::size_t i = 0; i < effects_list.size(); ++i) {
        const auto& e = effects_list[i];
        if (e.target != -2 || !near_screen(e.x_px, e.y_px)) continue;
        effects.push_back({3000 + int(i), e.x_px, e.y_px, mix_key(std::uint32_t(e.kind), std::uint32_t(e.frame))});
    }
    const auto& projectiles = combat_world.projectiles();
    for (std::size_t i = 0; i < projectiles.size(); ++i) {
        const auto& p = projectiles[i];
        int x = p.x * 16 + 8, y = p.y * 16 + 9; // i/y: TOP|LEFT from the tile anchor (+8, +9)
        if (p.duration == 2 && p.frame - 1 == 1) { x += combat::kDx[std::size_t(p.direction)] * 8; y += combat::kDy[std::size_t(p.direction)] * 8; }
        if (!near_screen(x, y)) continue;
        effects.push_back({2000 + int(i), x, y, mix_key(mix_key(std::uint32_t(p.effect + (p.from_hero ? 0x100 : 0)), std::uint32_t(p.frame)), std::uint32_t(p.direction))});
    }
    if (const auto& g = combat_world.guardian(); g.state != 0 && g.id >= 0) {
        const int x = g.x * 16 + 8, y = g.y * 16 + 8;
        if (near_screen(x, y))
            effects.push_back({4000, x, y, mix_key(mix_key(std::uint32_t(g.id << 8 | g.state), std::uint32_t(g.anim)), std::uint32_t(g.loops % 3 == 0) + 0x4000u)});
    }
    for (std::size_t i = 0; i < combat_world.effects().size(); ++i) {
        const auto& e = combat_world.effects()[i];
        const int x = e.x_px + 8, y = e.y_px + 9;
        if (near_screen(x, y)) effects.push_back({4001 + int(i), x, y, mix_key(std::uint32_t(e.kind << 8 | e.frame) + 0x4100u, std::uint32_t(combat_world.guardian().id))});
    }
    // Nearest to the hero first: they win the slots when more are on screen than there are slots.
    if (out.size() > 1)
        std::sort(out.begin() + 1, out.end(), [&](const Entity& a, const Entity& b) {
            const auto d = [&](const Entity& e) { return std::abs(e.ax - out[0].ax) + std::abs(e.ay - out[0].ay); };
            return d(a) < d(b);
        });
    // Effects are short-lived: they come right after the hero.
    out.insert(out.begin() + (out.empty() ? 0 : 1), effects.begin(), effects.end());
    if (out.size() > kSpriteSlots) out.resize(kSpriteSlots);
    return out;
}


// Bosses (maps 11, 13, 15, 82): /boss/{file} atlas shared by the encounter, stored under its first
// stats id (main.cpp boss_texture_base); frames up to 149x96, so they are drawn on their own layer.
int boss_texture_base(int stats_id) { return boss_texture_base_of(stats_id); }
std::unordered_map<int, BossSpriteAnimations> boss_animations;

void load_boss_art(const GameArchive& archive, int map_id) {
    boss_animations.clear();
    boss_runs.clear();
    const int file = map_id == 11 ? 1 : map_id == 13 ? 2 : map_id == 15 ? 3 : map_id == 82 ? 4 : 0;
    if (!file) return;
    static constexpr int first[5]{0, 32, 35, 39, 42}, last[5]{0, 32, 38, 41, 42};
    std::vector<std::size_t> ids;
    for (int id = first[file]; id <= last[file]; ++id) {
        try {
            auto animation = load_boss_animations(archive, file, id);
            const auto count = [&](std::size_t type, std::size_t dir) { return int(std::max<std::size_t>(1, animation.types[type][dir].size())); };
            combat::EnemyFrames counts;
            counts.idle = count(0, 0); counts.walk = count(1, 0);
            counts.death = count(2, 0); counts.attack = count(3, 0);
            for (std::size_t d = 0; d < 4; ++d) counts.attack_by_direction[d] = count(3, d);
            for (std::size_t set = 0; set < 3; ++set) counts.alt_attack[set] = count(3 + set, 0);
            for (const auto& type : animation.types)
                for (const auto& direction : type)
                    for (const auto& frame : direction)
                        for (const auto& layer : frame) ids.push_back(layer.frame);
            const std::string effect = "boss/atef/" + std::to_string(id);
            if (archive.contains(effect)) {
                auto eif = load_eif(archive, effect);
                for (const auto& frame : eif) for (const auto& layer : frame) if (layer.atlas_frame != 0xff) ids.push_back(layer.atlas_frame);
                counts.effect = std::max(1, int(eif.size()));
                character_art.enemy_effects[id] = std::move(eif);
            }
            combat_world.set_enemy_frames(id, counts);
            boss_animations.emplace(id, std::move(animation));
        } catch (const std::exception& error) {
            log_file("boss " + std::to_string(id) + ": " + error.what());
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    character_art.bytes += load_color_frames(archive, "boss/" + std::to_string(file), ids, 0x20000 + (first[file] << 8), character_art.frames);
}

// av.a(Graphics): walk +4, attack +12 (cc: its current set), death +8, else idle.
const std::vector<NpcSpriteLayer>* boss_frame(const combat::Enemy& enemy) {
    const auto boss = boss_animations.find(enemy.sprite);
    if (boss == boss_animations.end()) return nullptr;
    const bool attacking = enemy.state == combat::EnemyState::attack;
    int direction = enemy.direction;
    if (enemy.draw_direction && !(enemy.boss == combat::BossKind::ak && attacking)) direction = enemy.draw_direction;
    if (direction < 1 || direction > 4) return nullptr;
    std::size_t type = enemy.state == combat::EnemyState::walk ? 1 : enemy.state == combat::EnemyState::dying ? 2 : attacking ? 3 : 0;
    if (attacking && enemy.boss == combat::BossKind::cc) type = std::size_t(2 + std::clamp(enemy.pattern, 1, 3));
    const auto* frames = &boss->second.types[type][std::size_t(direction - 1)];
    if (frames->empty()) frames = &boss->second.types[0][std::size_t(direction - 1)];
    if (frames->empty()) return nullptr;
    const auto frame = std::size_t(std::max(0, enemy.frame));
    return &(*frames)[type == 2 ? std::min(frame, frames->size() - 1) : frame % frames->size()];
}

bool dual_arena() { return !boss_animations.empty() && world.map.height * 16 > kScreenH; }

// The boss layer: BG2 as an 8 bpp 256x256 bitmap with two pages in VRAM D (64 KB each, map bases
// 24 and 28), colours added to the field's BG palette. A boss change is composed in RAM, written to
// the hidden page and shown by switching the page in VBlank, so a frame is never seen half drawn.
// The title and the game-over screen use BG2 as a 16 bpp bitmap instead (set_bg2_bitmap16).
int bg_boss = -1;
// Frame profile (cpuGetTiming ticks, 33.5 MHz), summed over the diagnostics second.
u32 prof_logic = 0, prof_sprites = 0, prof_boss = 0, prof_ui = 0, prof_work = 0, prof_max = 0, prof_frame_start = 0;
int prof_boss_draws = 0, prof_arena_draws = 0;
u32 prof_arena_bg = 0, prof_arena_actors = 0, prof_arena_copy = 0;
bool boss_layer_on = false;
int boss_origin_x = 0, boss_origin_y = 0, boss_scroll_x = 0, boss_scroll_y = 0;
std::uint32_t boss_drawn_key = 0;
std::array<int, 4> boss_box{{0, 0, 0, 0}};
int boss_page = 0;             // page on screen: 0 = base 24, 1 = base 28
bool boss_flip_pending = false;
std::vector<std::uint8_t> boss_pixels;   // the page being composed, 256x256
std::array<std::array<int, 2>, 2> boss_page_rows{{{0, 256}, {0, 256}}}; // rows each VRAM page may hold
std::array<int, 2> boss_buffer_rows{{0, 256}};                        // rows boss_pixels may hold

void set_bg2_bitmap16(bool sixteen) {
    bg_boss = sixteen ? bgInit(2, BgType_Bmp16, BgSize_B16_256x256, 24, 0) : bgInit(2, BgType_Bmp8, BgSize_B8_256x256, 24, 0);
    bgSetPriority(bg_boss, 1);
    bgWrapOff(bg_boss);
    bgHide(bg_boss);
    boss_page = 0;
    boss_flip_pending = false;
    boss_drawn_key = 0;
    boss_page_rows = {{{0, 256}, {0, 256}}};
    boss_buffer_rows = {0, 256};
    boss_layer_on = false;
}

std::uint8_t boss_colour(u16 c) {
    c &= 0x7fff;
    if (colour_index.size() != 32768) colour_index.assign(32768, 0);
    if (const auto i = colour_index[c]) return i;
    const auto before = map_art.palette.colors.size();
    const auto i = map_art.palette.index(c);
    if (map_art.palette.colors.size() != before) BG_PALETTE[i] = c;
    colour_index[c] = i;
    return i;
}

// 16 bpp frame into the 256x256 8 bpp page through the colour table (misses add a palette entry).
HOT void blit_to8(std::uint8_t* dst, const u16* src, int w, int h, int x, int y, bool flip, const std::uint8_t* table) {
    const int y0 = y < 0 ? -y : 0, y1 = h < 256 - y ? h : 256 - y, x0 = x < 0 ? -x : 0, x1 = w < 256 - x ? w : 256 - x;
    if (x0 >= x1 || y0 >= y1) return;
    for (int yy = y0; yy < y1; ++yy) {
        std::uint8_t* d = dst + (y + yy) * 256 + x;
        const u16* row = src + yy * w;
        for (int xx = x0; xx < x1; ++xx) {
            const u16 v = flip ? row[w - 1 - xx] : row[xx];
            if (!v) continue;
            std::uint8_t i = table[v & 0x7fff];
            if (!i) i = boss_colour(v);
            d[xx] = i;
        }
    }
}

const FrameRuns& runs_of(const ColorFrame& f, bool want_idx) {
    auto& r = boss_runs[f.px.data()];
    if (r.row_start.empty()) {
        r.row_start.reserve(std::size_t(f.h) + 1);
        for (int y = 0; y < f.h; ++y) {
            r.row_start.push_back(u16(r.runs.size() / 2));
            const u16* row = f.px.data() + std::size_t(y) * f.w;
            for (int x = 0; x < f.w;) {
                if (!row[x]) { ++x; continue; }
                const int x0 = x;
                while (x < f.w && row[x]) ++x;
                r.runs.push_back(u16(x0)); r.runs.push_back(u16(x - x0));
            }
        }
        r.row_start.push_back(u16(r.runs.size() / 2));
    }
    if (want_idx && !r.has_idx) {
        r.idx.resize(f.px.size());
        for (std::size_t i = 0; i < f.px.size(); ++i) r.idx[i] = f.px[i] ? boss_colour(f.px[i]) : 0;
        r.has_idx = true;
    }
    return r;
}

// Opaque runs of a frame into dst (T = u16 for the bottom arena, u8 for the boss page), clipped.
template <typename T>
HOT void blit_runs(T* dst, int stride, int dw, int dh, const T* src, int w, int h, int x, int y, bool flip, const u16* row_start, const u16* runs) {
    const int y0 = y < 0 ? -y : 0, y1 = h < dh - y ? h : dh - y;
    for (int yy = y0; yy < y1; ++yy) {
        T* d = dst + (y + yy) * stride;
        const T* row = src + yy * w;
        for (int k = row_start[yy]; k < row_start[yy + 1]; ++k) {
            const int rx = runs[2 * k], len = runs[2 * k + 1];
            if (!flip) {
                const int a = std::max(0, x + rx), b = std::min(dw, x + rx + len);
                for (int px = a; px < b; ++px) d[px] = row[px - x];
            } else { // screen x = x + (w - 1 - sx) for source x sx
                const int a = std::max(0, x + w - rx - len), b = std::min(dw, x + w - rx);
                for (int px = a; px < b; ++px) d[px] = row[w - 1 - (px - x)];
            }
        }
    }
}

void blit_boss16(u16* dst, int stride, int dw, int dh, const ColorFrame& f, int x, int y, bool flip) {
    if (x >= dw || y >= dh || x + f.w <= 0 || y + f.h <= 0) return;
    const auto& r = runs_of(f, false);
    blit_runs<u16>(dst, stride, dw, dh, f.px.data(), f.w, f.h, x, y, flip, r.row_start.data(), r.runs.data());
}
void blit_boss8(std::uint8_t* dst, const ColorFrame& f, int x, int y, bool flip) {
    if (x >= 256 || y >= 256 || x + f.w <= 0 || y + f.h <= 0) return;
    const auto& r = runs_of(f, true);
    blit_runs<std::uint8_t>(dst, 256, 256, 256, r.idx.data(), f.w, f.h, x, y, flip, r.row_start.data(), r.runs.data());
}

void boss_flush() { // during VBlank: show the page composed last
    if (!boss_flip_pending) return;
    boss_flip_pending = false;
    boss_page ^= 1;
    bgSetMapBase(bg_boss, boss_page ? 28 : 24);
    if (boss_layer_on) bgShow(bg_boss);
}

void draw_boss_layer(int sx, int sy, bool hidden) {
    std::vector<std::size_t> bosses;
    const auto& live = combat_world.enemies();
    for (std::size_t i = 0; i < live.size(); ++i)
        if (live[i].boss != combat::BossKind::none && live[i].state != combat::EnemyState::dead && !live[i].hidden) bosses.push_back(i);
    if (bosses.empty() || hidden) {
        if (boss_layer_on) { bgHide(bg_boss); boss_layer_on = false; boss_drawn_key = 0; }
        return;
    }
    const auto anchor = [&](const combat::Enemy& e) {
        int x = e.x_px + 8 + (e.width - 1) * 8, y = e.y_px + 8 + e.draw_offset_y;
        if (e.shake == 3 || e.shake == 1) { x += combat::kDx[std::size_t(e.shake_direction)] * 3; y += combat::kDy[std::size_t(e.shake_direction)] * 3; }
        return std::make_pair(x, y);
    };
    // The layer follows the camera (32 px above and below the view), moved only past that margin.
    const int want_x = sx, want_y = sy - 32;
    if (boss_drawn_key == 0 || want_x != boss_origin_x || std::abs(want_y - boss_origin_y) > 24) {
        boss_origin_x = want_x;
        boss_origin_y = want_y;
        boss_drawn_key = 0;
    }
    std::uint32_t key = mix_key(std::uint32_t(boss_origin_x), std::uint32_t(boss_origin_y)) | 1u;
    for (const auto i : bosses) {
        const auto& e = live[i];
        const auto a = anchor(e);
        key = mix_key(key, mix_key(mix_key(std::uint32_t(e.state) << 8 | std::uint32_t(e.direction), std::uint32_t(e.frame) << 4 | std::uint32_t(e.pattern)),
                                   mix_key(std::uint32_t(a.first), std::uint32_t(a.second))));
        key = mix_key(key, attached_key(int(i)));
    }
    if (key != boss_drawn_key && !boss_flip_pending) {
        boss_drawn_key = key;
        ++prof_boss_draws;
        if (boss_pixels.size() != 256 * 256) { boss_pixels.assign(256 * 256, 0); boss_buffer_rows = {0, 0}; }
        if (colour_index.size() != 32768) colour_index.assign(32768, 0);
        if (boss_buffer_rows[1] > boss_buffer_rows[0])
            fill32(reinterpret_cast<u32*>(boss_pixels.data() + boss_buffer_rows[0] * 256), 0, (boss_buffer_rows[1] - boss_buffer_rows[0]) * 64);
        int x0 = 256, y0 = 256, x1 = 0, y1 = 0;
        for (const auto i : bosses) {
            const auto& e = live[i];
            const auto a = anchor(e);
            std::vector<Layer> layers;
            if (const auto* frame = boss_frame(e))
                for (const auto& layer : *frame)
                    if (const auto* f = art_frame(character_art.frames, 0x20000 + (boss_texture_base(e.sprite) << 8) + layer.frame))
                        layers.push_back({f, layer.x, layer.y, layer.mirrored});
            const std::size_t own = layers.size(); // the boss's own frames: cached runs
            const auto extra = attached_layers(int(i));
            layers.insert(layers.end(), extra.begin(), extra.end());
            for (std::size_t k = 0; k < layers.size(); ++k) {
                const auto& layer = layers[k];
                const auto& f = *layer.frame;
                const int left = a.first - boss_origin_x + layer.x, top = a.second - boss_origin_y + layer.y;
                x0 = std::min(x0, left); y0 = std::min(y0, top); x1 = std::max(x1, left + f.w); y1 = std::max(y1, top + f.h);
                if (k < own) blit_boss8(boss_pixels.data(), f, left, top, layer.flip);
                else blit_to8(boss_pixels.data(), f.px.data(), f.w, layer_h(layer), left, top, layer.flip, colour_index.data());
            }
        }
        boss_box = {x0, y0, x1, y1};
        // Into the page that is not on screen; boss_flush() switches to it in VBlank. Only the rows that
        // held the boss there before or hold it now are copied.
        const int r0 = std::clamp(y0, 0, 256), r1 = std::clamp(y1, r0, 256);
        boss_buffer_rows = {r0, r1};
        auto& page_rows = boss_page_rows[std::size_t(boss_page ^ 1)];
        const int c0 = page_rows[1] > page_rows[0] ? std::min(page_rows[0], r0) : r0;
        const int c1 = page_rows[1] > page_rows[0] ? std::max(page_rows[1], r1) : r1;
        page_rows = {r0, r1};
        u8* page = reinterpret_cast<u8*>(BG_GFX) + (boss_page ? 24 : 28) * 0x4000;
        if (c1 > c0) copy32(reinterpret_cast<u32*>(page + c0 * 256), reinterpret_cast<const u32*>(boss_pixels.data() + c0 * 256), (c1 - c0) * 64);
        boss_flip_pending = true;
    }
    boss_scroll_x = sx - boss_origin_x;
    boss_scroll_y = sy - boss_origin_y;
    boss_layer_on = true; // shown by boss_flush() with the first composed page
}

// Small 32x32 bitmap sprites: damage numbers and MISS (img/etcui 5..8 strips, 1), ground drops
// (item icons), all above the characters.
constexpr int kSmallSlots = 16;
std::array<SpriteSlot, kSmallSlots> small_slots;

// aw7 / r.a: two large frames (9x14, advance 8), then small ones (7x9, advance 6); MISS is etcui 1.
// Layers relative to the popup anchor (centre, top).
std::vector<Layer> number_layers(const combat::Popup& popup) {
    std::vector<Layer> layers;
    if (popup.kind == combat::PopupKind::miss) {
        const auto found = combat_art.find(1);
        if (found != combat_art.end()) layers.push_back({&found->second, -found->second.w / 2, 0, false});
        return layers;
    }
    const auto number = combat::damage_number_frame(popup.value, popup.age);
    const auto strip = combat_art.find(number.strip);
    static std::array<ColorFrame, 10> glyphs;
    if (strip == combat_art.end()) return layers;
    const std::string digits = std::to_string(std::abs(popup.value));
    const int start = 1 - int(digits.size()) * number.advance / 2;
    for (std::size_t i = 0; i < digits.size() && i < glyphs.size(); ++i) {
        auto& g = glyphs[i];
        g.w = number.width; g.h = number.height;
        g.px.assign(std::size_t(g.w) * g.h, 0);
        const int sx = (digits[i] - '0') * number.width;
        for (int y = 0; y < g.h && y < strip->second.h; ++y)
            for (int x = 0; x < g.w && sx + x < strip->second.w; ++x)
                g.px[std::size_t(y) * g.w + x] = strip->second.px[std::size_t(y) * strip->second.w + sx + x];
        layers.push_back({&g, start + int(i) * number.advance, 0, false});
    }
    return layers;
}

void compose_number(u16* gfx, const combat::Popup& popup) { compose(gfx, number_layers(popup), 32, 16, 8, 32); }

void draw_small(int sx, int sy, bool hidden, int first_oam) {
    struct Item { int x, y; std::uint32_t key; int kind; std::size_t index; };
    std::vector<Item> items;
    if (!hidden) {
        const auto hero = world.hero ? world.hero->state() : HeroState{};
        for (std::size_t i = 0; i < combat_world.popups().size(); ++i) {
            const auto& p = combat_world.popups()[i];
            int px = p.x_px, py = p.y_px;
            if (p.target == -1) { px = hero.x_pixels + 8; py = hero.y_pixels + 8; } // o.b: the owner's anchor
            else if (p.target >= 0 && std::size_t(p.target) < combat_world.enemies().size()) {
                const auto& owner = combat_world.enemies()[std::size_t(p.target)];
                if (owner.generation == p.generation && owner.state != combat::EnemyState::dead) {
                    px = owner.x_px + 8 + (owner.width - 1) * 8; py = owner.y_px + 8 + owner.draw_offset_y;
                }
            }
            items.push_back({px - 16, py - 30 - p.age * 4 - 8,
                             mix_key(mix_key(std::uint32_t(p.kind), std::uint32_t(p.value)), std::uint32_t(p.age < 2)), 0, i});
        }
        for (std::size_t i = 0; i < combat_world.drops().size(); ++i) {
            const auto& d = combat_world.drops()[i];
            if (d.ttl <= 16 && d.ttl % 3 == 0) continue; // blinks before it vanishes
            items.push_back({d.x * 16 + 8 - 16, d.y * 16 + 8 - 24, d.type == -1 ? 10u : 9u, 1, i});
        }
    }
    // A slot keeps what it showed last frame where it can: its graphics are written now, but the OAM
    // entry pointing at it changes only in VBlank, so a reused slot flashed a damage number as a drop.
    static std::array<int, kSmallSlots> idle{}; // frames a slot has been hidden (a blinking drop skips frames)
    std::array<bool, kSmallSlots> taken{};
    std::vector<int> slot_of(items.size(), -1);
    std::size_t shown = 0;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& item = items[i];
        if (item.x - sx < -32 || item.x - sx > kScreenW || item.y - sy < -32 || item.y - sy > kScreenH) continue;
        if (shown++ >= std::size_t(kSmallSlots)) break;
        slot_of[i] = -2; // shown, slot not chosen yet
        for (int k = 0; k < kSmallSlots; ++k)
            if (!taken[std::size_t(k)] && small_slots[std::size_t(k)].owner == item.kind &&
                small_slots[std::size_t(k)].key == item.key) { taken[std::size_t(k)] = true; slot_of[i] = k; break; }
    }
    for (int pass = 0; pass < 2; ++pass) // slots hidden for a while first
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (slot_of[i] != -2) continue;
            for (int k = 0; k < kSmallSlots; ++k)
                if (!taken[std::size_t(k)] && (pass == 1 || idle[std::size_t(k)] >= 2)) { taken[std::size_t(k)] = true; slot_of[i] = k; break; }
        }
    std::array<int, kSmallSlots> kind_of{};
    kind_of.fill(-1);
    for (std::size_t i = 0; i < items.size(); ++i) {
        const int k = slot_of[i];
        if (k < 0) continue;
        const auto& item = items[i];
        auto& slot = small_slots[std::size_t(k)];
        if (slot.key != item.key || slot.owner != item.kind) {
            if (item.kind == 0) compose_number(slot.gfx, combat_world.popups()[item.index]);
            else {
                const auto art = combat_art.find(int(item.key)); // etcui 9 chest, 10 gold
                std::vector<Layer> layers;
                if (art != combat_art.end()) layers.push_back({&art->second, -art->second.w / 2, -art->second.h, false});
                compose(slot.gfx, layers, 32, 16, 24, 32);
            }
            slot.key = item.key;
            slot.owner = item.kind;
        }
        // Popups over everything; drops on the ground: after the characters in OAM order, same priority.
        const int oam = item.kind == 0 ? first_oam + k : first_oam + kSmallSlots + kSpriteSlots + k;
        oamSet(&oamMain, oam, item.x - sx, item.y - sy, item.kind == 0 ? 0 : 1, 15, SpriteSize_32x32, SpriteColorFormat_Bmp, slot.gfx, -1,
               false, false, false, false, false);
        kind_of[std::size_t(k)] = item.kind;
    }
    for (int k = 0; k < kSmallSlots; ++k) {
        small_slots[std::size_t(k)].used = kind_of[std::size_t(k)] >= 0;
        idle[std::size_t(k)] = small_slots[std::size_t(k)].used ? 0 : std::min(idle[std::size_t(k)] + 1, 2);
        if (kind_of[std::size_t(k)] != 0)
            oamSet(&oamMain, first_oam + k, 0, 0, 0, 15, SpriteSize_32x32, SpriteColorFormat_Bmp, small_slots[0].gfx, -1, false, true,
                   false, false, false);
        if (kind_of[std::size_t(k)] != 1)
            oamSet(&oamMain, first_oam + kSmallSlots + kSpriteSlots + k, 0, 0, 1, 15, SpriteSize_32x32, SpriteColorFormat_Bmp,
                   small_slots[0].gfx, -1, false, true, false, false, false);
    }
}

void draw_entities(int sx, int sy, bool hidden) {
    auto entities = hidden ? std::vector<Entity>{} : visible_entities(sx, sy);
    for (auto& slot : slots) slot.used = false;
    std::vector<int> slot_of(entities.size(), -1);
    for (std::size_t e = 0; e < entities.size(); ++e) // keep the slot an entity had last frame
        for (int s = 0; s < kSpriteSlots; ++s)
            if (slots[s].owner == entities[e].owner && !slots[s].used) { slots[s].used = true; slot_of[e] = s; break; }
    for (std::size_t e = 0; e < entities.size(); ++e) {
        if (slot_of[e] >= 0) continue;
        for (int s = 0; s < kSpriteSlots; ++s)
            if (!slots[s].used) { slots[s].used = true; slots[s].owner = -1; slot_of[e] = s; break; }
    }
    std::vector<std::uint16_t> occluders;
    for (std::size_t e = 0; e < entities.size(); ++e) {
        auto& slot = slots[std::size_t(slot_of[e])];
        // Objects drawn after this actor in the original depth order that overlap its canvas.
        const int left = entities[e].ax - kAnchorX, top = entities[e].ay - kAnchorY;
        const int depth = entities[e].depth == INT_MIN ? entities[e].ay : entities[e].depth;
        occluders.clear();
        for (std::size_t k = 0; k < map_art.placed.size(); ++k) {
            const auto& o = map_art.placed[k];
            if (o.depth > depth && o.left < left + kCanvas && o.left + o.frame->w > left && o.top < top + kCanvas && o.top + o.frame->h > top)
                occluders.push_back(std::uint16_t(k));
        }
        auto key = entities[e].key;
        if (!occluders.empty()) key = mix_key(mix_key(key, std::uint32_t(left) << 16 ^ std::uint32_t(top)), std::uint32_t(occluders.size()) | 0x8000u);
        if (slot.owner != entities[e].owner || slot.key != key) {
            compose(slot.gfx, entity_layers(entities[e].owner), kCanvas, kAnchorX, kAnchorY, kCanvas, occluders.empty() ? nullptr : &occluders, left, top);
            slot.owner = entities[e].owner;
            slot.key = key;
        }
    }
    for (auto& slot : slots) if (!slot.used) slot.owner = -1;
    // Depth: lower on the screen is in front; OAM entries after the small sprites, lower index on top.
    std::vector<std::size_t> order(entities.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return entities[a].ay > entities[b].ay; });
    const int first = kSmallSlots;
    for (std::size_t n = 0; n < order.size(); ++n) {
        const auto& e = entities[order[n]];
        const int ox = e.ax - sx - kAnchorX, oy = e.ay - sy - kAnchorY;
        // OAM y wraps at 256 and x at 512: a sprite below the screen (y 192..255) would show its tail at the top.
        const bool off = oy >= kScreenH || oy <= -64 || ox >= kScreenW || ox <= -64;
        oamSet(&oamMain, first + int(n), off ? 0 : ox, off ? 0 : oy, 1, 15, SpriteSize_64x64, SpriteColorFormat_Bmp,
               slots[std::size_t(slot_of[order[n]])].gfx, -1, false, off, false, false, false);
    }
    for (int n = int(order.size()); n < kSpriteSlots; ++n)
        oamSet(&oamMain, first + n, 0, 0, 1, 15, SpriteSize_64x64, SpriteColorFormat_Bmp, slots[0].gfx, -1, false, true, false, false, false);
    draw_small(sx, sy, hidden, 0);
}

// ---------------------------------------------------------------------------------------------
// Map loading screen, as the original: n request 1 -> bu.b() ("- MAP", 200 steps) -> r.c(Graphics).
// Three logic ticks of black 4 px bands every 12 rows close over the field (cumulative, r.m 0..2),
// then black with NOW LOADING (bh.p), the animated dots, the two-line frame and a 2 px progress
// bar (r.k / r.l), and Hands-On Mobile (bh.o) below. The bands are BG0 tiles above the sprites;
// the loading screen is drawn into the field bitmap, which is redrawn for the new map anyway.

int bg_overlay = -1;
bool field_shown = false;  // field bitmap + sprites on screen
bool pending_show = false; // show the field once the new map's first window is drawn
BitmapFont ui_font;
std::vector<std::string> com_text;
int loading_k = 0, loading_m = 0;
constexpr int kLoadingSteps = 200;
constexpr int kOverlayBlack = 15 * 16 + 1; // BG0 palette bank 15, colour 1

std::string com(std::size_t index, const char* fallback) {
    return index < com_text.size() && !com_text[index].empty() ? com_text[index] : fallback;
}

void show_field(bool show) {
    if (show) { bgShow(bg_main); REG_DISPCNT |= DISPLAY_SPR_ACTIVE; if (boss_layer_on) bgShow(bg_boss); }
    else { bgHide(bg_main); REG_DISPCNT &= ~DISPLAY_SPR_ACTIVE; if (bg_boss >= 0) bgHide(bg_boss); }
    field_shown = show;
}

void init_overlay() {
    bg_overlay = bgInit(0, BgType_Text4bpp, BgSize_T_256x256, 0, 1); // map 0x06000000, tiles 0x06004000 (VRAM E)
    bgSetPriority(bg_overlay, 0);
    bgSetPriority(bg_main, 2); // sprites use priority 1: between the field and the bands
    u32* tiles = reinterpret_cast<u32*>(bgGetGfxPtr(bg_overlay)); // 4 bpp: one word per tile row
    for (int i = 0; i < 8; ++i) tiles[i] = 0;
    for (int phase = 0; phase < 3; ++phase)
        for (int r = 0; r < 3; ++r) // 24 px period = 3 tile rows
            for (int py = 0; py < 8; ++py)
                tiles[(1 + phase * 3 + r) * 8 + py] = ((r * 8 + py) % 12) < 4 * (phase + 1) ? 0x11111111u : 0u;
    dmaFillHalfWords(0, bgGetMapPtr(bg_overlay), 32 * 32 * 2);
    bgHide(bg_overlay);
}

void hide_caption();
void close_field() {
    hide_caption();
    if (!field_shown) return;
    BG_PALETTE[kOverlayBlack] = RGB15(0, 0, 0);
    u16* map = bgGetMapPtr(bg_overlay);
    bgShow(bg_overlay);
    for (int phase = 0; phase < 3; ++phase) {
        for (int ty = 0; ty < 32; ++ty)
            for (int tx = 0; tx < 32; ++tx) map[ty * 32 + tx] = u16((15 << 12) | (1 + phase * 3 + ty % 3));
        for (int f = 0; f < 4; ++f) swiWaitForVBlank(); // one logic tick (71 ms) per step
    }
}

void ring_pixel(int x, int y, std::uint8_t value) {
    if (x < 0 || y < 0 || x >= kRingW || y >= kRingH) return;
    u16& cell = bgGetGfxPtr(bg_main)[(y * kRingW + x) >> 1];
    cell = (x & 1) ? u16((cell & 0x00ff) | (value << 8)) : u16((cell & 0xff00) | value);
}

void ring_rect(int x, int y, int w, int h, std::uint8_t value) {
    for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx) ring_pixel(xx, yy, value);
}

int text_width(const std::string& text) {
    int width = 0;
    for (const auto& glyph : layout_bitmap_text(ui_font, text, 0, 0, 4096))
        width = std::max(width, glyph.x + int(glyph.source_width));
    return width;
}

void ring_text(const std::string& text, int x, int y, std::uint8_t value) {
    const auto& atlas = ui_font.atlas;
    if (atlas.width == 0) return;
    const int rows = std::min<int>(ui_font.height, int(atlas.height));
    for (const auto& glyph : layout_bitmap_text(ui_font, text, 0, 0, 4096))
        for (int yy = 0; yy < rows; ++yy)
            for (int xx = 0; xx < int(glyph.source_width); ++xx) {
                const auto* p = &atlas.rgba[(std::size_t(yy) * atlas.width + glyph.source_x + xx) * 4];
                if (p[3] < 128) continue;
                const std::uint32_t rgb = std::uint32_t(p[0]) << 16 | std::uint32_t(p[1]) << 8 | p[2];
                if (rgb == 0u || (ui_font.has_shadow_palette && rgb == ui_font.shadow_palette_rgb)) continue;
                ring_pixel(x + glyph.x + xx, y + glyph.y + yy, value);
            }
}

void draw_loading_progress() {
    const int i = kScreenW / 2, j = kScreenH / 2;
    ring_rect(i + 20, j - 12, 18, 10, 0);
    ring_text(std::string("...").substr(0, std::size_t(loading_m % 4)), i + 20, j - 12, 1);
    const int width = 95 * std::min(loading_k, kLoadingSteps) / kLoadingSteps;
    ring_rect(i - 48, j + 2, width, 1, 3);
    ring_rect(i - 48, j + 3, width, 1, 4);
}

void begin_loading() {
    show_field(false);
    bgHide(bg_overlay);
    dmaFillHalfWords(0, bgGetGfxPtr(bg_main), kRingW * kRingH);
    BG_PALETTE[0] = RGB15(0, 0, 0);
    BG_PALETTE[1] = RGB15(0xdf >> 3, 0xbf >> 3, 0x7f >> 3); // 14663551
    BG_PALETTE[2] = RGB15(0x9f >> 3, 0x7f >> 3, 0x3f >> 3); // 10452799
    BG_PALETTE[3] = RGB15(0xdf >> 3, 0x9f >> 3, 0x3f >> 3); // 14655295
    BG_PALETTE[4] = RGB15(0xff >> 3, 0xff >> 3, 0x7f >> 3); // 16777087
    bgSetScroll(bg_main, 0, 0);
    bgUpdate();
    const int i = kScreenW / 2, j = kScreenH / 2;
    ring_text(com(37, "NOW LOADING"), i - 48, j - 12, 1);
    ring_rect(i - 50, j, 99, 1, 1);
    ring_rect(i - 51, j + 1, 2, 2, 1);
    ring_rect(i + 48, j + 1, 2, 2, 1);
    ring_rect(i - 50, j + 5, 99, 1, 2);
    ring_rect(i - 51, j + 3, 2, 2, 2);
    ring_rect(i + 48, j + 3, 2, 2, 2);
    const std::string vendor = com(38, "Hands-On Mobile");
    ring_text(vendor, i - text_width(vendor) / 2, j + 50, 2);
    loading_k = 0;
    loading_m = 3;
    draw_loading_progress();
    bgShow(bg_main);
}

void loading_progress(int k) { loading_k = k; ++loading_m; draw_loading_progress(); }

// Text on the black screen (SCR_DEL hides the world, then TALKTEXT, e.g. "3 years later..."):
// drawn centred on the top screen in BG0 4 bpp tiles (palette bank 14), the field layer hidden.
constexpr int kCaptionTile = 16, kCaptionRows = 8, kCaptionTop = 8; // tile rows 8..15 (y 64..127)
std::string caption_shown;
bool field_hidden_by_event = false;

void put_caption_pixel(int x, int y, int value) {
    if (x < 0 || y < 0 || x >= 256 || y >= kCaptionRows * 8) return;
    const int tile = kCaptionTile + (y / 8) * 32 + x / 8;
    u16* row = bgGetGfxPtr(bg_overlay) + tile * 16 + (y % 8) * 2; // 4 bpp: 2 halfwords per row
    u16& cell = row[(x % 8) / 4];
    const int shift = (x % 4) * 4;
    cell = u16((cell & ~(0xf << shift)) | (value << shift));
}

void show_caption(const std::string& text) {
    if (text == caption_shown) return;
    caption_shown = text;
    dmaFillHalfWords(0, bgGetGfxPtr(bg_overlay) + kCaptionTile * 16, kCaptionRows * 32 * 32);
    const auto glyphs = layout_bitmap_text(ui_font, text, 0, 0, 232);
    int lines = 0, widest = 0;
    std::vector<int> line_width(16, 0);
    for (const auto& g : glyphs) {
        const int line = g.y / std::max(1, int(ui_font.height));
        lines = std::max(lines, line + 1);
        if (line < 16) line_width[std::size_t(line)] = std::max(line_width[std::size_t(line)], g.x + int(g.source_width));
        widest = std::max(widest, g.x + int(g.source_width));
    }
    const int fh = std::max(1, int(ui_font.height));
    const int top = (kCaptionRows * 8 - lines * fh) / 2;
    const auto& atlas = ui_font.atlas;
    const int rows = std::min<int>(fh, int(atlas.height));
    for (const auto& g : glyphs) {
        const int line = g.y / fh;
        const int left = (256 - (line < 16 ? line_width[std::size_t(line)] : widest)) / 2;
        for (int yy = 0; yy < rows; ++yy)
            for (int xx = 0; xx < int(g.source_width); ++xx) {
                const auto* px = &atlas.rgba[(std::size_t(yy) * atlas.width + g.source_x + xx) * 4];
                if (px[3] < 128) continue;
                const bool shadow = px[0] < 64 && px[1] < 64 && px[2] < 64;
                put_caption_pixel(left + g.x + xx, top + g.y + yy, shadow ? 2 : 1);
            }
    }
    BG_PALETTE[14 * 16 + 1] = RGB15(31, 19, 0); // dialogue gold 0xff9f00
    BG_PALETTE[14 * 16 + 2] = RGB15(0, 0, 0);
    u16* map = bgGetMapPtr(bg_overlay);
    for (int ty = 0; ty < 32; ++ty)
        for (int tx = 0; tx < 32; ++tx)
            map[ty * 32 + tx] = ty >= kCaptionTop && ty < kCaptionTop + kCaptionRows
                                    ? u16((14 << 12) | (kCaptionTile + (ty - kCaptionTop) * 32 + tx)) : 0;
    bgShow(bg_overlay);
}

void hide_caption() {
    if (caption_shown.empty()) return;
    caption_shown.clear();
    dmaFillHalfWords(0, bgGetMapPtr(bg_overlay), 32 * 32 * 2);
    bgHide(bg_overlay);
    map_art.palette.upload(); // bank 14 overlaps the field's colours 225..226
}

// ---------------------------------------------------------------------------------------------
// Map loading.

void rebuild_combat();

void place_near(int x, int y) { // nearest walkable tile (debug map switching has no destination)
    const int w = world.map.width, h = world.map.height;
    int best_x = x, best_y = y, best_d = 1 << 30;
    for (int ty = 0; ty < h; ++ty)
        for (int tx = 0; tx < w; ++tx) {
            const auto index = std::size_t(ty) * w + tx;
            if (index >= world.event.overlay.size() || std::int8_t(world.event.overlay[index]) < 0) continue;
            const int d = (tx - x) * (tx - x) + (ty - y) * (ty - y);
            if (d < best_d) { best_d = d; best_x = tx; best_y = ty; }
        }
    world.hero->place_on_tile(best_x, best_y);
}

// x/y < 0: nearest walkable tile to the centre. facing: actor direction 1..4.
bool load_world(const GameArchive& archive, int map_id, int x, int y, int facing) {
    close_field();
    begin_loading();
    const u32 start = vblanks;
    try {
        GameMap map = load_map(archive, map_id);
        EventGrid event = load_event_grid(archive, game.zone, map_id, map);
        if (const auto setup = select_setup_group(event, game.switches)) apply_setup_group(map, event, setup->second);
        loading_progress(50);
        world.map = std::move(map);
        world.event = std::move(event);
        world.map_id = map_id;
        world.objects = decode_objects(world.event);
        world.npcs = decode_npcs(world.event);
        enemy_records = decode_enemies(world.event);
        world.enemies = enemy_records;
        world.npc_motion = make_npc_motion_states(world.event);
        world.hero = std::make_unique<HeroInput>(world.map, world.event, world.npcs, world.enemies);
        world.visuals = make_event_visual_state(world.event);
        world.last_auto_cell.reset();
        world.peaceful = world.map.kind == 1 || world.map.kind == 5 || world.map.kind == 9 || world.map.kind == 15;
        if (x >= 0 && y >= 0) world.hero->place_on_tile(x, y); else place_near(world.map.width / 2, world.map.height / 2);
        world.hero->apply_script_command(6, std::uint8_t(facing));
        loading_progress(80);
        prepare_map(archive);
        loading_progress(140);
        load_character_art(archive);
        rebuild_combat();
        deferred_attack = 0;
        loading_progress(kLoadingSteps);
        redraw_ring();
        for (auto& slot : slots) slot.owner = -1;
        for (auto& slot : small_slots) slot.owner = -1;
    } catch (const std::exception& error) {
        log_file("map " + std::to_string(map_id) + ": " + error.what());
        map_art.palette.upload(); // the old map stays: redraw it
        redraw_ring();
        bgHide(bg_main);
        pending_show = true;
        return false;
    }
    map_art.palette.upload();
    bgHide(bg_main); // until the first window of the new map is drawn
    autosave_pending = true;
    pending_show = true;
    world.load_ms = int((vblanks - start) * 1000 / 60);
    sample_memory();
    char text[256];
    std::snprintf(text, sizeof(text),
                  "map %d zone %s kind %u %ux%u: load %d ms, terrain %d obj %d colors %d, objects %u npcs %u enemies %u, "
                  "character art %u KB, heap used %u KB peak %u KB free %u KB",
                  world.map_id, game.zone.c_str(), unsigned(world.map.kind), unsigned(world.map.width),
                  unsigned(world.map.height), world.load_ms, world.terrain_frames, world.object_frames, world.colors,
                  unsigned(world.objects.size()), unsigned(world.npcs.size()), unsigned(world.enemies.size()),
                  unsigned(character_art.bytes / 1024), unsigned(memory.used / 1024), unsigned(memory.peak / 1024),
                  unsigned(memory.free / 1024));
    log_file(text);
    ui_dirty = true;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Events (main.cpp try_start_event / the logic tick's event pump).

bool has_item(std::uint8_t type, std::uint8_t id, std::uint8_t count) { return has_inventory_requirement(game.inventory, type, id, count); }

void persist_event() {
    game.switches = ev.runtime->switch_bits();
    game.quests = ev.runtime->quest_bits();
    game.inventory = ev.runtime->inventory();
    game.money = ev.runtime->money();
    game.vitals = ev.runtime->vitals();
    game.guardians = ev.runtime->guardians();
    game.progress = ev.runtime->progress();
}

void end_event() {
    if (ev.runtime) persist_event();
    world.visuals.world_hidden = false;
    ev = EventState{};
    ui_dirty = true;
}

void start_event_actions(std::size_t action_list, const char* why, int trigger) {
    ev = EventState{};
    ev.actions = world.event.actions[action_list];
    ev.runtime = std::make_unique<EventRuntime>(world.map, world.event, game.switches, game.quests, game.inventory, game.money,
                                                game.vitals, game.guardians, game.progress);
    ev.runtime->camera.x = world.hero->state().x_pixels; // the first frame already follows the hero
    ev.runtime->camera.y = world.hero->state().y_pixels;
    world.visuals.world_hidden = false;
    log_file(std::string("event ") + why + " trigger " + std::to_string(trigger) + " actions " + std::to_string(action_list));
    ui_dirty = true;
}

void try_start_event(int x, int y, EventTriggerType type) {
    if (ev.runtime || world.hero->state().motion_mode != 1) return;
    if (x < 0 || y < 0 || x >= world.map.width || y >= world.map.height) return;
    const auto raw = world.event.overlay[std::size_t(y) * world.map.width + x];
    const int signed_id = raw >= 128 ? int(raw) - 256 : raw;
    const int trigger_id = type == EventTriggerType::actor_contact ? signed_id : std::abs(signed_id);
    if (trigger_id < 1 || trigger_id > 127) return;
    const auto match = find_event_trigger(world.event, trigger_id, type, game.switches, game.quests, has_item);
    if (!match || match->action_list >= world.event.actions.size()) return;
    static const char* const names[] = {"contact", "on-tile", "facing", "stop-facing"};
    start_event_actions(match->action_list, names[int(type) & 3], trigger_id);
}

constexpr int kDx[5]{0, 0, 0, -1, 1}, kDy[5]{0, -1, 1, 0, 0};

bool interaction_requested = false;
void try_start_event(int x, int y, EventTriggerType type);
bool try_pickup();
void start_attack(bool skill, bool walking);
void try_button_event() {
    if (ev.runtime) return;
    const auto hero = world.hero->state();
    if (hero.motion_mode != 1) { interaction_requested = true; world.hero->release(); return; }
    const bool pressed_while_walking = interaction_requested;
    interaction_requested = false;
    if (hero.direction < 1 || hero.direction > 4) return;
    if (try_pickup()) return;
    try_start_event(hero.x_tile, hero.y_tile, EventTriggerType::hero_on_tile);
    if (!ev.runtime) try_start_event(hero.x_tile + kDx[hero.direction], hero.y_tile + kDy[hero.direction], EventTriggerType::hero_facing_tile);
    if (!ev.runtime) start_attack(false, pressed_while_walking);
}

void show_text(std::string text, bool choice) {
    ev.text = format_dialogue_text(std::move(text));
    ev.page = 0;
    ev.choice = choice;
    ev.dialogue = !choice;
    ui_dirty = true;
}

void map_transition(const GameArchive& archive, const MapTransitionRequest& request) {
    persist_event(); // ah opcode 7 discards the script after i(): keep its mutations
    ev.runtime.reset();
    ev = EventState{};
    if (!load_world(archive, request.target_map_id, request.hero_x, request.hero_y, request.destination_facing + 1))
        ev.status = "Map transition failed";
}

bool pump_event(const GameArchive& archive) { // true: FLASH/SHAKE holds the world (ah state 5)
    bool effect = false;
    for (unsigned steps = 0; steps < 64 && ev.runtime && ev.status.empty(); ++steps) {
        EventStepResult result;
        try {
            result = ev.runtime->step(ev.actions, ev.choice_response, ev.advance, steps == 0 ? 1 : 0);
        } catch (const std::exception& error) {
            log_file(std::string("event error: ") + error.what());
            end_event();
            return false;
        }
        ev.choice_response.reset();
        ev.advance = false;
        const auto& c = result.command;
        effect = result.status == EventStepStatus::visual_command && (c.opcode == 24 || c.opcode == 25);
        if (c.opcode == 8 && result.status == EventStepStatus::applied) {
            const auto hero = world.hero->state(); // ao.a(SS): to the tile, motion and facing kept
            world.hero->displace(c.arg1 * 16 - hero.x_pixels, c.arg2 * 16 - hero.y_pixels);
        } else if (c.opcode == 41 && result.status == EventStepStatus::applied) {
            const auto index = std::size_t(c.arg1);
            if (index < world.npcs.size()) {
                const auto old = world.npcs[index];
                auto decoded = decode_npcs(world.event);
                if (index < decoded.size()) {
                    world.npcs[index] = decoded[index];
                    world.npcs[index].x = old.x; world.npcs[index].y = old.y;
                    world.npcs[index].partial_x = old.partial_x; world.npcs[index].partial_y = old.partial_y;
                    try { load_character_art(archive); } catch (const std::exception& error) { log_file(error.what()); }
                    for (auto& slot : slots) slot.owner = -1;
                }
            }
        } else if (c.opcode == 42) { // terrain cell rewritten by the script
            const auto cell = std::size_t(c.arg2) * world.map.width + c.arg1;
            if (cell < world.map.terrain.size() && cell < map_art.cells.size()) {
                try { ensure_terrain_frames(archive, {world.map.terrain[cell]}); } catch (const std::exception& error) { log_file(error.what()); }
                map_art.palette.upload();
                map_art.cells[cell] = world.map.terrain[cell];
                if (cell_in_window(c.arg1, c.arg2)) render_cell(c.arg1, c.arg2);
            }
        } else if (c.opcode == 40) { // object visibility
            world.objects = decode_objects(world.event);
            try { rebuild_objects(archive); } catch (const std::exception& error) { log_file(error.what()); }
            map_art.palette.upload();
            redraw_ring();
        }

        if (result.status == EventStepStatus::wait && c.opcode == 1) {
            if (!ev.dialogue) {
                show_text(resolve_event_text(world.event, c.arg1, text_bank).value_or("[missing event text]"), false);
                if (const auto index = event_text_bank_index(world.event, c.arg1)) mark_recipe(game.recipes, recipe_slot(*index)); // port: recipe journal
                ev.item_message = false;
                ev.portrait = c.arg2 < 128 ? int(c.arg2) : int(c.arg2) - 256;
            }
            ev.choice = false;
        } else if (result.status == EventStepStatus::wait && c.opcode == 2) {
            if (!ev.choice) { ev.choice_yes = true; ev.portrait = 0; show_text(resolve_event_text(world.event, c.arg1, text_bank).value_or("Make your choice."), true); }
        } else if (result.status == EventStepStatus::wait && c.opcode == 11) {
            if (!ev.dialogue) {
                const auto index = ev.runtime->item_message_string_index();
                show_text(index < text_bank.size() ? text_bank[index] : (index == 3938 ? tr("Inventory is full.", "Сумка полна.") : tr("No item.", "Нет предмета.")), false);
                ev.item_message = true;
                ev.portrait = 0;
            }
            ev.choice = false;
        } else if ((c.opcode == 1 || c.opcode == 2 || c.opcode == 11) && result.status != EventStepStatus::wait) {
            ev.dialogue = ev.choice = false;
            ev.text.clear();
            ui_dirty = true;
        } else if (result.status == EventStepStatus::actor_command) {
            if (c.opcode >= 15 && c.opcode <= 17) apply_npc_movement_command(c, world.npc_motion);
            else if (!world.hero->apply_script_command(c.opcode, c.arg1)) ev.status = "Unsupported actor movement";
        } else if (result.status == EventStepStatus::visual_command) {
            // EMO_HERO: o.c() replaces the hero's attached effects first.
            if (c.opcode == 49) combat_world.clear_attached_effects(-1);
            apply_event_visual_command(world.visuals, c, result.remaining_ticks);
        } else if (result.status == EventStepStatus::audio_command) {
            // ah BGM_PLAY: 5..7 event music (bw.c), 8 a WAV effect; BGM_STOP ends the event music.
            if (c.opcode == 26) {
                if (c.arg1 >= 5 && c.arg1 <= 7) snd::play_music(snd::event_music, c.arg1);
                else if (c.arg1 == 8) snd::play_sound(c.arg1);
            } else if (c.opcode == 27) snd::stop_music(snd::event_music);
        } else if (result.status == EventStepStatus::delegated) {
            ev.status = "Unsupported event command " + std::to_string(c.opcode);
        } else if (result.status == EventStepStatus::map_transition) {
            if (!result.transition || result.transition->destination_facing > 3) ev.status = "Map transition has no destination";
            else { map_transition(archive, *result.transition); return false; }
        } else if (result.status == EventStepStatus::ending) {
            end_event();
            start_ending(archive);
            return false;
        } else if (result.status == EventStepStatus::screen_transition) {
            const auto screen = result.screen.value_or(EventScreen::shop);
            end_event();
            if (item_catalog.empty()) try { item_catalog = load_item_catalog(archive); } catch (const std::exception&) {}
            if (screen == EventScreen::shop) open_shop(archive); else open_service(screen);
            world.hero->release();
            ui_dirty = true;
            return false;
        } else if (result.status == EventStepStatus::ended) {
            end_event();
            return false;
        }
        if (!ev.status.empty()) { log_file(ev.status); ui_dirty = true; }
        if (!ev.runtime || !event_step_continues_frame(result, ev.actions)) break;
    }
    return effect;
}

// ---------------------------------------------------------------------------------------------
// Field combat (main.cpp rebuild_combat / sync_live_enemies / combat_view / start_attack).

bool game_over = false; // hero died: the game-over screen, then the title
int game_over_frames = 0; // as.e: draws left (16 .. 1)

void sync_live_enemies() { // live enemy tiles feed hero/NPC collision; frame_index = combat slot
    world.enemies.clear();
    const auto& live = combat_world.enemies();
    for (std::size_t i = 0; i < live.size(); ++i) {
        if (live[i].state == combat::EnemyState::dead) continue;
        MapEntity entity{};
        entity.x = std::uint8_t(live[i].tile_x);
        entity.y = std::uint8_t(live[i].tile_y);
        entity.code = std::uint8_t(live[i].sprite);
        entity.resource_slot = 0;
        entity.frame_index = int(i);
        entity.partial_x = live[i].partial_x;
        entity.partial_y = live[i].partial_y;
        world.enemies.push_back(entity);
        for (int extra = 1; extra < live[i].width; ++extra) { // second column of 2-wide enemies blocks too
            MapEntity tail = entity;
            tail.x = std::uint8_t(entity.x + extra);
            tail.resource_slot = -1;
            tail.frame_index = -1;
            world.enemies.push_back(tail);
        }
    }
}

void rebuild_combat() {
    std::vector<std::int8_t> overlay(world.event.overlay.size());
    for (std::size_t i = 0; i < overlay.size(); ++i) overlay[i] = std::int8_t(world.event.overlay[i]);
    combat_world.reset(world.map.width, world.map.height, std::move(overlay), &enemy_stats, vblanks * 2654435761u + 1);
    combat_world.restart_regen(game.progress.level); // n.f -> ao.a
    for (int bank = 0; bank < 3; ++bank) combat_world.set_death_effect_frames(bank, int(death_eif[std::size_t(bank)].size()));
    std::vector<std::pair<int, int>> npc_tiles; // ac.g registers visible NPCs before ae.d places enemies
    for (std::size_t i = 0; i < world.npcs.size(); ++i)
        if (i >= world.event.npc_visible.size() || world.event.npc_visible[i]) npc_tiles.emplace_back(world.npcs[i].x, world.npcs[i].y);
    combat_world.set_blocked_tiles(std::move(npc_tiles));
    for (const auto& record : enemy_records) {
        const int id = record.code >= 128 ? int(record.code) - 256 : record.code;
        if (record.resource_slot < 0 || id < 0 || std::size_t(id) >= enemy_stats.size()) continue;
        const auto found = character_art.enemy_animations.find(id);
        if (found != character_art.enemy_animations.end())
            combat_world.set_enemy_frames(id, {int(std::max<std::size_t>(1, found->second.idle[0].size())),
                                               int(std::max<std::size_t>(1, found->second.walk[0].size())),
                                               int(std::max<std::size_t>(1, found->second.attack[0].size()))});
        if (const auto effect = character_art.enemy_effects.find(id); effect != character_art.enemy_effects.end() && found != character_art.enemy_animations.end()) {
            combat::EnemyFrames counts{int(std::max<std::size_t>(1, found->second.idle[0].size())),
                                       int(std::max<std::size_t>(1, found->second.walk[0].size())),
                                       int(std::max<std::size_t>(1, found->second.attack[0].size()))};
            counts.effect = std::max(1, int(effect->second.size()));
            combat_world.set_enemy_frames(id, counts);
        }
        combat_world.queue_spawn(id, id, record.x, record.y, 0);
    }
    load_boss_art(*current_archive, world.map_id);
    combat_world.spawn_bosses(world.map_id);
    sync_live_enemies();
}

combat::HeroCombatView combat_view() {
    combat::HeroCombatView view;
    const auto hero = world.hero->state();
    view.x_tile = hero.x_tile; view.y_tile = hero.y_tile;
    view.partial_x = hero.partial_x; view.partial_y = hero.partial_y;
    view.direction = hero.direction;
    view.character_class = game.character_class;
    view.level = game.progress.level;
    view.hp = game.vitals.hp; view.max_hp = game.vitals.max_hp;
    view.sp = game.vitals.sp; view.max_sp = game.vitals.max_sp;
    view.agility = game.progress.attributes[2];
    view.base_will = game.progress.attributes[3];
    view.moving = hero.motion_mode == 2;
    view.max_combo = game.progress.max_combo;
    view.combat_map = !world.peaceful;
    for (std::size_t i = 0; i < 3; ++i)
        if (!hero_art.magic[i].empty()) view.magic_frames[i] = int(hero_art.magic[i].size());
    if (game.guardians.equipped < game.guardians.slots.size())
        if (const auto& active = game.guardians.slots[game.guardians.equipped]) {
            view.guardian_id = active->id; view.guardian_level = active->level;
            view.guardian_calm = active->calm; view.guardian_wild = active->wild;
        }
    // n.e(int): quest 29 grants the sword hero a second combo step.
    if (game.character_class == 6 && game.quests.size() > 3 && (game.quests[3] >> 5 & 1)) view.max_combo = std::max(view.max_combo, 2);
    const auto gear = gear_totals(game.equipment, item_catalog);
    view.attack = combat::hero_base_attack(game.progress.attributes[0] + gear.bonus[0]) + gear.weapon;
    view.defense = combat::hero_base_defense(game.progress.attributes[0] + gear.bonus[0], game.progress.level) + gear.defense;
    view.agility += gear.bonus[2];
    view.hp_attribute = game.progress.attributes[1] + gear.bonus[1];
    view.will = game.progress.attributes[3] + gear.bonus[3];
    view.accessory_evasion = gear.armour_refine;
    view.weapon_refine = game.equipment[0] ? game.equipment[0]->refine : 0;
    for (std::size_t slot = 0; slot < game.equipment.size(); ++slot) {
        if (!game.equipment[slot] || item_catalog.empty()) continue;
        const auto& item_stack = *game.equipment[slot];
        if (const auto* item = find_catalog_item(item_catalog, item_stack.item_type, item_stack.item_id))
            if (const auto info = equipment_info(*item)) {
                if (slot == 0) {
                    view.weapon_power = info->power;
                    if (const auto properties = weapon_properties(*item)) {
                        view.weapon_critical_chance = properties->critical_chance;
                        view.weapon_critical_bonus_tenths = properties->critical_bonus_tenths;
                        view.weapon_hit_option = item_option(item_stack, item);
                    }
                } else if (slot == 1) view.shield_hit_option = item_option(item_stack, item);
            }
    }
    return view;
}

void roll_gear(InventoryStack& stack) { // ad.a(type,id,true,true): new gear rolls its bonuses
    if (const auto* item = find_catalog_item(item_catalog, stack.item_type, stack.item_id))
        roll_equipment(stack, *item, true, [](int lo, int hi) { return combat_world.rand_range(lo, hi); });
}

void start_attack(bool skill, bool walking) {
    const auto hero = world.hero->state();
    if (hero.partial_x || hero.partial_y) { deferred_attack = skill ? 2 : 1; world.hero->release(); return; }
    int face = hero.direction;
    // n.a(boolean): from standing the hero turns toward a side enemy; queued while walking.
    if (combat_world.request_attack(combat_view(), skill, walking ? nullptr : &face) && face != world.hero->state().direction)
        world.hero->apply_script_command(6, std::uint8_t(face));
}

void try_skill_attack() {
    if (ev.runtime || game_over) return;
    if (world.hero->state().motion_mode != 1) world.hero->release();
    start_attack(true, world.hero->state().motion_mode == 2);
}

std::string notice; // short field message on the bottom screen (pickups, full bag)
u32 notice_until = 0;

// A press on the field: pick up a drop, else an event on/in front of the hero, else attack.
bool try_pickup() {
    combat::CombatEvents picked;
    const auto add_drop_item = [&](int type, int id, bool apply) {
        const auto t = std::int8_t(type), i = std::int8_t(id);
        if (is_stackable_item(std::uint8_t(type)))
            for (auto& stack : game.inventory)
                if (stack.item_type == t && stack.item_id == i && stack.count + 1 <= inventory_stack_limit) {
                    if (apply) { ++stack.count; consolidate_stacks(game.inventory); }
                    return true;
                }
        if (game.inventory.size() >= inventory_slot_limit) return false;
        if (apply) { // n: dropped gear rolls bonuses; unidentified only when e.a allows it
            game.inventory.push_back({t, i, 1});
            auto& added = game.inventory.back();
            roll_gear(added);
            if (const auto* item = find_catalog_item(item_catalog, t, i))
                if (const auto info = equipment_info(*item); info && !info->flag) added.identified = true;
            consolidate_stacks(game.inventory);
        }
        return true;
    };
    if (combat_world.try_pickup(combat_view(), picked, [&](int t, int i) { return add_drop_item(t, i, false); })) {
        if (picked.gold > 0) {
            game.money += picked.gold;
            notice = tr("Got ", "Получено ") + std::to_string(picked.gold) + " " + // bank 3948: "G" / "Зол", as main.cpp
                     (text_bank.size() > 3948 && !text_bank[3948].empty() ? text_bank[3948] : std::string("G"));
        }
        for (const auto& item : picked.items) {
            add_drop_item(item.first, item.second, true);
            const auto* found = find_catalog_item(item_catalog, item.first, item.second);
            std::size_t index = 0; // the key is a decimal index into the language bank
            bool numeric = found && !found->name_key.empty();
            if (numeric) for (const unsigned char ch : found->name_key) { if (ch < '0' || ch > '9') { numeric = false; break; } index = index * 10 + (ch - '0'); }
            notice = tr("Got ", "Получено: ") + (numeric && index < text_bank.size() ? text_bank[index] : tr("item", "предмет"));
        }
        notice_until = vblanks + 100;
        return true;
    }
    if (!picked.messages.empty()) { notice = tr("Inventory is full.", "Сумка полна."); notice_until = vblanks + 100; }
    return false;
}

void sync_npc_tiles(const std::vector<std::size_t>& moved) {
    for (const auto index : moved)
        if (index < world.npcs.size()) {
            world.npcs[index].x = std::uint8_t(world.npc_motion[index].x_tile);
            world.npcs[index].y = std::uint8_t(world.npc_motion[index].y_tile);
            world.npcs[index].partial_x = world.npc_motion[index].partial_x;
            world.npcs[index].partial_y = world.npc_motion[index].partial_y;
        }
}

void animate_characters() {
    for (std::size_t i = 0; i < world.npcs.size(); ++i) {
        const int code = world.npcs[i].code >= 128 ? int(world.npcs[i].code) - 256 : world.npcs[i].code;
        const auto animations = character_art.npc_animations.find(code);
        if (world.npcs[i].frame_index >= 0 || animations == character_art.npc_animations.end()) continue;
        auto& m = world.npc_motion[i];
        int direction = m.direction < 1 || m.direction > 4 ? 2 : m.direction;
        const auto& walk = animations->second.walk[std::size_t(direction - 1)];
        const auto& seq = m.motion_mode == 2 && !walk.empty() ? walk : animations->second.idle[std::size_t(direction - 1)];
        if (seq.size() > 1) m.animation_frame = std::uint8_t((m.animation_frame + 1) % seq.size());
    }
}

void start_trigger_event(int trigger_id) { // ah.a(byte): a boss death starts a type-0 event list
    if (ev.runtime) return;
    const auto match = find_event_trigger(world.event, trigger_id, EventTriggerType::actor_contact, game.switches, game.quests, has_item);
    if (match && match->action_list < world.event.actions.size()) start_event_actions(match->action_list, "combat", trigger_id);
}

void combat_field_tick() {
    std::vector<std::pair<int, int>> npc_tiles; // ae.c() NPC steps only run in events: NPCs stay put here
    for (std::size_t i = 0; i < world.npcs.size(); ++i)
        if (i >= world.event.npc_visible.size() || world.event.npc_visible[i]) {
            npc_tiles.emplace_back(world.npcs[i].x, world.npcs[i].y);
            if (world.npcs[i].partial_y) npc_tiles.emplace_back(world.npcs[i].x, world.npcs[i].y + 1);
            else if (world.npcs[i].partial_x) npc_tiles.emplace_back(world.npcs[i].x + 1, world.npcs[i].y);
        }
    combat_world.set_blocked_tiles(std::move(npc_tiles));
    auto view = combat_view();
    combat_world.set_visible_area(view_x, view_y, view_x + kScreenW, view_y + kScreenH - 21); // as.b = H-21
    combat::CombatEvents events;
    const int hp_before = game.vitals.hp, sp_before = game.vitals.sp, level_before = game.progress.level;
    combat_world.tick(view, events);
    for (const int sound : combat_world.take_sounds()) snd::play_sound(sound);
    game.vitals.hp = view.hp;
    game.vitals.sp = view.sp;
    if (events.experience > 0) add_event_experience(game.progress, game.vitals, game.guardians, unsigned(events.experience));
    if (game.progress.level != level_before) combat_world.add_level_up_effects();
    if (combat_world.hero_dash_offset() > 0) { // ao.b: the sword skill dash moves the hero 1-2 tiles forward
        const auto hero = world.hero->state();
        const int dash = combat_world.hero_dash_offset();
        const int x = hero.x_tile + combat::kDx[std::size_t(hero.direction)] * dash;
        const int y = hero.y_tile + combat::kDy[std::size_t(hero.direction)] * dash;
        if (x >= 0 && y >= 0 && x < world.map.width && y < world.map.height) {
            world.hero->place_on_tile(x, y);
            world.hero->apply_script_command(6, std::uint8_t(hero.direction));
        }
    }
    for (const auto& write : events.overlay_writes)
        world.event.overlay[std::size_t(write.y) * world.map.width + std::size_t(write.x)] = std::uint8_t(write.value);
    if (events.push_x || events.push_y) world.hero->displace(events.push_x * 16, events.push_y * 16);
    if (events.start_trigger) start_trigger_event(events.start_trigger);
    if (events.hero_died && !game_over) { // ao.q -> n.a(16): the game-over screen, then the title
        game_over = true;
        game_over_frames = 17;
        ui_dirty = true;
        snd::stop_all(); snd::play_sound(12); // n.e(16): bw.f(), then the game-over jingle
        log_file("game over on map " + std::to_string(world.map_id));
    }
    sync_live_enemies();
    if (game.vitals.hp != hp_before || game.vitals.sp != sp_before || events.experience > 0) ui_row_dirty = true;
}

void logic_tick(const GameArchive& archive) {
    if (ev.runtime) {
        auto& camera = ev.runtime->camera; // ah.b(): detached camera pans, otherwise follows the hero
        if (camera.follow) { camera.x = world.hero->state().x_pixels; camera.y = world.hero->state().y_pixels; }
        else camera.tick();
        bool effect = false;
        if (ev.status.empty()) effect = pump_event(archive);
        if (ev.runtime && !ev.dialogue && !ev.choice && !effect && ev.status.empty()) {
            world.hero->sync_world(world.event, world.npcs, world.enemies);
            world.hero->tick();
            { // the event owns its copies of vitals/progress/roster until it ends
                const auto saved_progress = game.progress;
                const auto saved_guardians = game.guardians;
                game.progress = ev.runtime->progress();
                game.guardians = ev.runtime->guardians();
                auto view = combat_view();
                game.progress = saved_progress;
                game.guardians = saved_guardians;
                auto vitals = ev.runtime->vitals();
                view.hp = vitals.hp; view.sp = vitals.sp;
                combat::CombatEvents control;
                combat_world.event_tick(view, control);
                for (const int sound : combat_world.take_sounds()) snd::play_sound(sound);
                vitals.hp = view.hp; vitals.sp = view.sp;
                ev.runtime->set_vitals(vitals);
            }
            sync_npc_tiles(tick_npc_movement(world.npc_motion, world.map, world.event, world.enemies, world.hero->state()));
            world.hero->sync_world(world.event, world.npcs, world.enemies);
        }
    } else if (ev.status.empty()) {
        world.hero->sync_world(world.event, world.npcs, world.enemies);
        const auto before = world.hero->state();
        if (!combat_world.hero_attacking()) {
            world.hero->tick();
            if (world.hero->blocked()) { // walking into an enemy targets it
                const auto h = world.hero->state();
                combat_world.select_contact(h.x_tile + combat::kDx[std::size_t(h.direction)], h.y_tile + combat::kDy[std::size_t(h.direction)]);
            }
        }
        if (deferred_attack && !world.hero->state().partial_x && !world.hero->state().partial_y) {
            const bool skill = deferred_attack == 2;
            deferred_attack = 0;
            world.hero->apply_script_command(5, 0); // stop, then attack from the tile
            start_attack(skill, true);
        }
        const auto after = world.hero->state();
        if (after.motion_mode == 1 && interaction_requested) try_button_event();
        bool auto_check = false;
        if (after.motion_mode == 1 && !combat_world.hero_attacking()) {
            const auto cell = std::size_t(after.y_tile) * world.map.width + std::size_t(after.x_tile);
            if (!world.last_auto_cell || *world.last_auto_cell != cell) { world.last_auto_cell = cell; auto_check = true; }
        }
        if (!ev.runtime && before.motion_mode == 2 && after.motion_mode == 1 && after.direction >= 1 && after.direction <= 4)
            try_start_event(after.x_tile + kDx[after.direction], after.y_tile + kDy[after.direction], EventTriggerType::hero_stop_facing_tile);
        if (!ev.runtime && auto_check) try_start_event(after.x_tile, after.y_tile, EventTriggerType::actor_contact);
        if (!ev.runtime) combat_field_tick();
        if (world.hero) world.hero->sync_world(world.event, world.npcs, world.enemies);
    }
    animate_characters();
    if (dual_arena()) arena_dirty = true; // the bottom half of the arena moves with the tick
}

// ---------------------------------------------------------------------------------------------
// Bottom screen (sub engine, 16 bpp bitmap): the 3DS bottom-screen layout (draw_bottom_screen)
// fitted to 256x192. Middle area: minimap outdoors, a plate with the location name indoors, or the
// dialogue with its portrait; then the HP/SP row with the EXP line and five touch buttons (calm and
// wild guardian skill, menu, quick item, next item). During dialogue the button row answers it
// (Next/Skip, Yes/No, Ok). Drawn in RAM and copied to VRAM when something changed.

namespace ui {
constexpr u16 C(std::uint32_t rgb) { return u16(RGB15((rgb >> 19) & 31, (rgb >> 11) & 31, (rgb >> 3) & 31) | 0x8000); }
constexpr u16 plum = C(0x221220), panel = C(0x341a2c), shade = C(0x140a12), bronze = C(0xc49a5a), bronze_mid = C(0x8c6438),
              bronze_dark = C(0x4a301e), ink = C(0xece0c8), gold = C(0xff9f00), black = C(0x000000), selected = C(0x5f1f3f);
constexpr int kW = 256, kH = 192;
constexpr int kAreaTop = 18, kAreaH = 110;      // middle area
constexpr int kRowY = 131;                      // HP/SP row
constexpr int kButtonsY = 156, kButtonsH = 33;  // button row
struct Rect { int x, y, w, h; };
struct Hit { Rect r; int action; };
enum Action { kNext = 1, kSkip, kYes, kNo, kOk, kCalm, kWild, kMenu, kUseItem, kNextItem };

u16 back[kW * kH];
u16* vram = nullptr;
std::vector<Hit> hits;
BitmapFont& font = ui_font;                        // fonts/small.mf, text white / shadow black
std::unordered_map<int, ColorFrame>& art = ui_art;
std::vector<std::string> location_names;
int quick_item_type = 7;
int fps = 0;

void fill(Rect r, u16 c) {
    const int x0 = std::max(0, r.x), y0 = std::max(0, r.y), x1 = std::min(kW, r.x + r.w), y1 = std::min(kH, r.y + r.h);
    for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x) back[y * kW + x] = c;
}
void frame(Rect r, u16 inside = panel) { // dark rim, bronze ring lit top-left, panel fill
    fill(r, bronze_dark);
    fill({r.x + 1, r.y + 1, r.w - 2, r.h - 2}, bronze_mid);
    fill({r.x + 1, r.y + 1, r.w - 2, 1}, bronze);
    fill({r.x + 1, r.y + 1, 1, r.h - 2}, bronze);
    fill({r.x + 2, r.y + 2, r.w - 4, r.h - 4}, bronze_dark);
    fill({r.x + 3, r.y + 3, r.w - 6, r.h - 6}, inside);
}
void bar(Rect r, int value, int maximum, u16 colour, u16 light) {
    fill(r, black);
    const int w = maximum > 0 ? int(std::int64_t(std::clamp(value, 0, maximum)) * r.w / maximum) : 0;
    if (w <= 0) return;
    fill({r.x, r.y, w, r.h}, colour);
    fill({r.x, r.y + 1, w, 1}, light);
}
int text_width(const std::string& text) {
    int w = 0;
    for (const auto& g : layout_bitmap_text(font, text, 0, 0, 100000)) w = std::max(w, g.x + int(g.source_width));
    return w;
}
int fh() { return std::max(1, int(font.height)); }
// Glyphs from `first_line` on, at most `lines` lines (text wrapped at max_w).
void text(const std::string& s, int x, int y, u16 colour, int max_w = 100000, int first_line = 0, int lines = 1000) {
    const auto& atlas = font.atlas;
    if (!atlas.width) return;
    const int rows = std::min<int>(fh(), int(atlas.height));
    for (const auto& g : layout_bitmap_text(font, s, 0, 0, max_w)) {
        const int line = g.y / fh();
        if (line < first_line || line >= first_line + lines) continue;
        const int gy = y + (line - first_line) * fh();
        for (int yy = 0; yy < rows; ++yy)
            for (int xx = 0; xx < int(g.source_width); ++xx) {
                const auto* p = &atlas.rgba[(std::size_t(yy) * atlas.width + g.source_x + xx) * 4];
                if (p[3] < 128) continue;
                const int px = x + g.x + xx, py = gy + yy;
                if (px < 0 || py < 0 || px >= kW || py >= kH) continue;
                const bool shadow = p[0] < 64 && p[1] < 64 && p[2] < 64;
                back[py * kW + px] = shadow ? black : colour;
            }
    }
}
int line_count(const std::string& s, int max_w) {
    int lines = 0;
    for (const auto& g : layout_bitmap_text(font, s, 0, 0, max_w)) lines = std::max(lines, g.y / fh() + 1);
    return lines;
}
void blit(const ColorFrame& f, int x, int y, bool flip = false) {
    for (int yy = 0; yy < f.h; ++yy)
        for (int xx = 0; xx < f.w; ++xx) {
            const u16 v = f.px[std::size_t(yy) * f.w + (flip ? f.w - 1 - xx : xx)];
            const int px = x + xx, py = y + yy;
            if (v && px >= 0 && py >= 0 && px < kW && py < kH) back[py * kW + px] = v;
        }
}
bool icon(int key, Rect r) {
    const auto found = art.find(key);
    if (found == art.end()) return false;
    blit(found->second, r.x + (r.w - found->second.w) / 2, r.y + (r.h - found->second.h) / 2);
    return true;
}
Rect button_rect(int i) { return {3 + i * 50, kButtonsY, 50 - 2, kButtonsH}; }

constexpr std::uint32_t kMinimapColours[30] = { // main.cpp: ae.a, two per map type (open, blocked)
    0xFFDF3F, 0x3F1F3F, 0xFFDF3F, 0x3F1F3F, 0xFFDFBF, 0x7F3F3F, 0xBFFF3F, 0x1F3F3F, 0x000000, 0x000000,
    0xBFFF3F, 0x1F3F3F, 0x3FBFFF, 0x1F3F3F, 0xBFFF3F, 0x1F3F3F, 0xBFFF3F, 0x1F3F3F, 0xBFFF3F, 0x1F3F3F,
    0x000000, 0x000000, 0xFFDFBF, 0x7F3F3F, 0xDFFFFF, 0x1F3F7F, 0xDFFFFF, 0x1F3F7F, 0xFFDFBF, 0x7F3F3F};

// Minimap: one colour per cell; trigger conditions are evaluated once per trigger id, not per cell.
// While it is on screen a hero step repaints just the two marker cells (move_hero_marker).
struct Minimap { int x0{}, y0{}, cell{1}, w{}, h{}, hero_x{-1}, hero_y{-1}; bool shown{}; std::vector<u16> colours; } mini;

void copy_to_vram(Rect r) {
    const int x0 = std::max(0, r.x), y0 = std::max(0, r.y), x1 = std::min(kW, r.x + r.w), y1 = std::min(kH, r.y + r.h);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) vram[y * 256 + x] = back[y * kW + x];
}

void draw_minimap() {
    const int w = world.map.width, h = world.map.height;
    if (w <= 0 || h <= 0) return;
    mini.w = w; mini.h = h;
    mini.cell = std::max(1, std::min(240 / w, (kAreaH - 8) / h));
    mini.x0 = kW / 2 - w * mini.cell / 2; mini.y0 = kAreaTop + (kAreaH - h * mini.cell) / 2;
    frame({mini.x0 - 4, mini.y0 - 4, w * mini.cell + 8, h * mini.cell + 8}, black);
    const auto& bag = ev.runtime ? ev.runtime->inventory() : game.inventory;
    const auto& switches = ev.runtime ? ev.runtime->switch_bits() : game.switches;
    const auto& quests = ev.runtime ? ev.runtime->quest_bits() : game.quests;
    std::array<std::int8_t, 128> active{}; // 0 unknown, 1 active, -1 not
    mini.colours.assign(std::size_t(w) * h, 0);
    for (std::size_t i = 0; i < mini.colours.size(); ++i) {
        const int c = i < world.event.overlay.size() ? int(std::int8_t(world.event.overlay[i])) : 0;
        std::uint32_t colour = kMinimapColours[std::min(29, world.map.kind * 2 + (c < 0 ? 1 : 0))];
        if (c != 0 && c != -128) {
            auto& known = active[std::size_t(std::abs(c)) & 127];
            if (!known)
                known = any_event_condition_met(world.event, std::abs(c), switches, quests,
                    [&](std::uint8_t a, std::uint8_t b, std::uint8_t n) { return has_inventory_requirement(bag, a, b, n); }) ? 1 : -1;
            if (known > 0) colour = world.map.kind == 6 ? 0xFF3FBF : 0x3F7FFF;
        }
        mini.colours[i] = C(colour);
        fill({mini.x0 + int(i % w) * mini.cell, mini.y0 + int(i / w) * mini.cell, mini.cell, mini.cell}, mini.colours[i]);
    }
    const auto hero = world.hero ? world.hero->state() : HeroState{};
    mini.hero_x = hero.x_tile; mini.hero_y = hero.y_tile;
    if (hero.x_tile >= 0 && hero.y_tile >= 0 && hero.x_tile < w && hero.y_tile < h)
        fill({mini.x0 + hero.x_tile * mini.cell, mini.y0 + hero.y_tile * mini.cell, mini.cell, mini.cell}, C(0xFF3F3F));
    mini.shown = true;
}

void move_hero_marker(int x, int y) {
    if (!mini.shown || (x == mini.hero_x && y == mini.hero_y)) return;
    const auto paint = [&](int cx, int cy, u16 colour) {
        if (cx < 0 || cy < 0 || cx >= mini.w || cy >= mini.h) return;
        const Rect r{mini.x0 + cx * mini.cell, mini.y0 + cy * mini.cell, mini.cell, mini.cell};
        fill(r, colour);
        copy_to_vram(r);
    };
    if (mini.hero_x >= 0 && mini.hero_y >= 0 && mini.hero_x < mini.w && mini.hero_y < mini.h)
        paint(mini.hero_x, mini.hero_y, mini.colours[std::size_t(mini.hero_y) * mini.w + mini.hero_x]);
    paint(x, y, C(0xFF3F3F));
    mini.hero_x = x; mini.hero_y = y;
}

std::string location_name() {
    int id = world.map_id;
    if (game.character_class == 8 && id == 65) id = 85; // ae.a special name
    return id >= 0 && std::size_t(id) < location_names.size() ? location_names[std::size_t(id)] : "";
}

void draw_location_plate() {
    const std::string name = location_name();
    if (name.empty()) return;
    const int tw = text_width(name), bw = std::clamp(tw + 28, 110, 240), bh = fh() + 34;
    const int bx = kW / 2 - bw / 2, by = kAreaTop + (kAreaH - bh) / 2;
    frame({bx, by, bw, bh});
    text(name, kW / 2 - tw / 2, by + 7, ink);
    const int dx = kW / 2 - 8, dy = by + fh() + 11; // stone arch and wooden door, as the 3DS plate
    const u16 stone = C(0x968c6e), stone_dark = C(0x5a5440), wood = C(0x60381e), wood_dark = C(0x3a2012);
    fill({dx + 3, dy, 10, 2}, stone); fill({dx + 1, dy + 2, 14, 2}, stone); fill({dx, dy + 4, 16, 12}, stone);
    fill({dx + 3, dy + 3, 10, 13}, stone_dark); fill({dx + 4, dy + 4, 8, 12}, wood);
    fill({dx + 6, dy + 4, 1, 12}, wood_dark); fill({dx + 9, dy + 4, 1, 12}, wood_dark); fill({dx - 2, dy + 15, 20, 2}, stone_dark);
}

// Dialogue: black framed box over the middle area, portrait (m/face) at the speaker's side.
constexpr int kDialogueTextW = 178;
int dialogue_rows() { return std::max(1, (kAreaH - 14) / fh()); }
int dialogue_text_w(bool portrait) { return portrait ? kDialogueTextW : kW - 22; }

void draw_dialogue() {
    const Rect box{3, kAreaTop - 2, kW - 6, kAreaH + 6};
    frame(box, black);
    int text_x = box.x + 8;
    const bool has_portrait = ev.portrait != 0 && art.count(0x100 + std::abs(ev.portrait) - 1);
    if (has_portrait) {
        const auto& face = art.at(0x100 + std::abs(ev.portrait) - 1);
        const int px = ev.portrait > 0 ? box.x + 5 : box.x + box.w - 5 - face.w;
        blit(face, px, box.y + box.h - 4 - face.h);
        if (ev.portrait > 0) text_x = box.x + 5 + face.w + 6;
    }
    const int w = dialogue_text_w(has_portrait);
    if (ev.choice || ev.item_message) {
        const int lines = line_count(ev.text, w);
        text(ev.text, text_x, box.y + (box.h - lines * fh()) / 2, ink, w);
    } else {
        text(ev.text, text_x, box.y + 7, gold, w, ev.page * dialogue_rows(), dialogue_rows());
        if (line_count(ev.text, w) > (ev.page + 1) * dialogue_rows()) fill({box.x + box.w - 10, box.y + box.h - 9, 5, 3}, gold);
    }
}

void draw_key_buttons(const std::vector<std::pair<std::string, int>>& keys, int highlighted) {
    fill({3, kButtonsY - 1, kW - 6, kButtonsH + 2}, plum);
    const int n = int(keys.size()), gap = 4, w = (kW - 8 - (n - 1) * gap) / n;
    for (int i = 0; i < n; ++i) {
        const Rect b{4 + i * (w + gap), kButtonsY + 4, w, 24};
        frame(b, i == highlighted ? selected : panel);
        const int tw = text_width(keys[std::size_t(i)].first);
        text(keys[std::size_t(i)].first, b.x + (b.w - tw) / 2, b.y + (b.h - fh()) / 2, ink);
        hits.push_back({{b.x, kButtonsY - 2, b.w, kButtonsH + 4}, keys[std::size_t(i)].second});
    }
}

std::string bank(std::size_t i, const char* fallback) { return i < text_bank.size() && !text_bank[i].empty() ? text_bank[i] : fallback; }

void draw_hud_row() {
    const auto& v = ev.runtime ? ev.runtime->vitals() : game.vitals;
    const auto& progress = ev.runtime ? ev.runtime->progress() : game.progress;
    frame({3, kRowY, kW - 6, 23});
    const int ty = kRowY + 5;
    text("HP", 9, ty, ink);
    bar({25, kRowY + 7, 44, 5}, v.hp, v.max_hp, C(0xe83428), C(0xff8c5a));
    std::string n = std::to_string(std::max(0, v.hp)) + "/" + std::to_string(std::max(0, v.max_hp));
    text(n, 124 - text_width(n), ty, ink);
    fill({128, kRowY + 4, 1, 14}, bronze_mid);
    text("SP", 133, ty, ink);
    bar({149, kRowY + 7, 44, 5}, v.sp, v.max_sp, C(0x28c8a0), C(0x78ffd2));
    n = std::to_string(std::max(0, v.sp)) + "/" + std::to_string(std::max(0, v.max_sp));
    text(n, 248 - text_width(n), ty, ink);
    const int next = std::max(1, progress.next_experience());
    fill({7, kRowY + 19, kW - 14, 1}, shade);
    fill({7, kRowY + 19, int(std::int64_t(std::clamp(progress.experience, 0, next)) * (kW - 14) / next), 1}, C(0xbfbf7f));
}

void draw_buttons() {
    const auto& guardians = ev.runtime ? ev.runtime->guardians() : game.guardians;
    const Guardian* active = guardians.equipped < guardians.slots.size() && guardians.slots[guardians.equipped]
                                 ? &*guardians.slots[guardians.equipped] : nullptr;
    for (int slot = 0; slot < 2; ++slot) {
        const Rect b = button_rect(slot);
        frame(b);
        const int skill = active ? (slot == 0 ? active->calm : active->wild) : -1;
        if (skill >= 0) {
            icon(0x500 + 6 + active->id * 4 + skill, b);
            // as.a: a 0xBF1F3F frame while p.c[skill] runs and ce.C (img/etcui 11) over 13 x 13*p.c/p.b.
            const int left = combat_world.guardian_cooldown(active->id, skill), full = combat::CombatWorld::guardian_cooldown_full(active->id, skill);
            const auto found = art.find(0x500 + 6 + active->id * 4 + skill);
            if (left > 0 && full > 0 && found != art.end()) {
                const int ix = b.x + (b.w - found->second.w) / 2, iy = b.y + (b.h - found->second.h) / 2;
                const u16 red = C(0xbf1f3f);
                fill({ix - 1, iy - 1, found->second.w + 2, 1}, red); fill({ix - 1, iy + found->second.h, found->second.w + 2, 1}, red);
                fill({ix - 1, iy - 1, 1, found->second.h + 2}, red); fill({ix + found->second.w, iy - 1, 1, found->second.h + 2}, red);
                const int shown = 13 * left / full;
                if (const auto* cover = art_frame(combat_art, 11))
                    for (int y = 0; y < shown && y + 1 < cover->h; ++y)
                        for (int x = 0; x < 13 && x + 1 < cover->w; ++x)
                            if (const u16 v = cover->px[std::size_t(y + 1) * cover->w + x + 1]) back[(iy + 1 + y) * kW + ix + 1 + x] = v;
            }
        }
        const std::string key = slot == 0 ? "B" : "Y"; // the DS key in the corner, as the 3DS labels
        text(key, b.x + b.w - text_width(key) - 4, b.y + b.h - fh() - 2, bronze);
        hits.push_back({b, slot == 0 ? kCalm : kWild});
    }
    {
        const Rect b = button_rect(2);
        frame(b);
        for (int k = 0; k < 3; ++k) fill({b.x + 14, b.y + 6 + k * 4, 20, 2}, ink);
        const std::string word = tr("MENU", "МЕНЮ");
        text(word, b.x + (b.w - text_width(word)) / 2, b.y + 19, ink);
        const auto& progress = ev.runtime ? ev.runtime->progress() : game.progress;
        if (progress.attribute_points > 0) // steady (the 3DS port too): blinking repainted the buttons every few frames
            if (const auto* stat = art_frame(combat_art, 0x3fe)) blit(*stat, b.x + (b.w - stat->w) / 2, b.y + 3);
        hits.push_back({b, kMenu});
    }
    {
        const Rect b = button_rect(3);
        frame(b);
        const auto& bag = ev.runtime ? ev.runtime->inventory() : game.inventory;
        icon(0x200 + quick_item_type, {b.x - 5, b.y - 2, b.w, b.h});
        int count = 0;
        if (const auto slot = quick_item_slot(bag, quick_item_type)) count = bag[*slot].count;
        const std::string c = std::to_string(count);
        const int w = text_width(c);
        const Rect box{b.x + b.w - w - 10, b.y + b.h - fh() - 8, w + 6, fh() + 4};
        frame(box, shade);
        text(c, box.x + 3, box.y + 2, ink);
        text("L", b.x + b.w - text_width("L") - 4, b.y + 2, bronze);
        hits.push_back({b, kUseItem});
    }
    {
        const Rect b = button_rect(4);
        frame(b);
        const int cx = b.x + b.w / 2 - 4, cy = b.y + b.h / 2;
        for (int k = 0; k < 8; ++k) fill({cx + k, cy - 8 + k, 1, 17 - 2 * k}, ink);
        text("R", b.x + b.w - text_width("R") - 4, b.y + b.h - fh() - 2, bronze);
        hits.push_back({b, kNextItem});
    }
}

void draw_status_line();
void draw_boss_bar(int y);
void draw_game_menu();
void draw_shop();
void draw_service();
bool full_screen_ui() { // the ending, a shop / service or the game menu own the whole bottom screen (see draw())
    return ending_running() || ((screen_open() || game_menu) && ev.status.empty() && !ev.dialogue && !ev.choice);
}
void update_status_strip() {
    if (full_screen_ui()) return;
    if (dual_arena() && !ev.dialogue && !ev.choice && ev.status.empty() && !game_menu) { // the arena owns the top: the boss bar instead
        draw_boss_bar(120);
        copy_to_vram({3, 120, kW - 6, 11});
        return;
    }
    fill({2, 2, kW - 4, 14}, plum);
    draw_status_line();
    copy_to_vram({2, 2, kW - 4, 14});
}
void draw_hud_row();
void update_row() { // HP/SP/EXP row and the status strip (enemy bar)
    if (full_screen_ui()) return;
    draw_hud_row();
    copy_to_vram({3, kRowY, kW - 6, 23});
    update_status_strip();
}
void update_status() { // status line only (notices expire)
    if (full_screen_ui()) return; // no empty strip over the menu
    if (dual_arena() && !ev.dialogue && !ev.choice && ev.status.empty() && !game_menu) return; // no status line over the arena
    fill({2, 2, kW - 4, 14}, plum);
    draw_status_line();
    copy_to_vram({2, 2, kW - 4, 14});
}
void draw_enemy_bar(const combat::Enemy& e) { // the 3DS enemy bar, in the status strip
    const auto& st = enemy_stats[std::size_t(e.stats)];
    const std::string name = std::size_t(st.name_text) < text_bank.size() ? text_bank[std::size_t(st.name_text)] : "?";
    text(name + "  " + bank(3949, "LV") + " " + std::to_string(st.level), 6, 4, ink);
    bar({150, 7, 60, 5}, e.hp, st.hp, C(0xe83428), C(0xff8c5a));
    const std::string n = std::to_string(std::max(0, e.hp));
    text(n, 248 - text_width(n), 4, ink);
}
void draw_status_line() {
    // p.a(Graphics) while p.i == 2: the skill name with the channel progress bar.
    if (const auto& g = combat_world.guardian(); g.state == 2 && g.id >= 0 && g.loops_total > 0) {
        const std::size_t name = std::size_t(g.id * 8 + g.skill * 2);
        if (name < guardian_skill_names.size()) text(guardian_skill_names[name], 6, 4, gold);
        bar({150, 9, 98, 3}, g.loops_total - g.loops + 1, g.loops_total, C(0xff5555), C(0xff9f9f));
        return;
    }
    if (const auto* target = combat_world.target_enemy(); target && target->stats >= 0 && std::size_t(target->stats) < enemy_stats.size()) {
        draw_enemy_bar(*target);
        return;
    }
    if (!notice.empty() && std::int32_t(notice_until - vblanks) > 0) text(notice, 6, 4, ink);
}

// Boss arenas are 240x320, made for the phone's 240x320 screen. On DS they span both screens: the
// top screen shows map rows [oy, oy+192), the bottom screen the next kArenaH rows, drawn here in
// software once per logic tick (positions only change on ticks), above the boss bar, the HP/SP row
// and the five buttons (no minimap).
constexpr int kArenaH = 120;

void blit_arena(const ColorFrame& f, int x, int y, bool flip, int rows = 0) {
    blit16(back, kW, kW, kArenaH, f.px.data(), f.w, rows > 0 && rows < f.h ? rows : f.h, x, y, flip);
}

// Terrain and objects of the whole arena map in 16 bpp, built once (rebuilt when the ring is redrawn).
int arena_bg_w = 0, arena_bg_h = 0;
const void* arena_bg_key = nullptr;
std::size_t arena_bg_objects = 0;

bool build_arena_bg() {
    const int w = world.map.width * 16, h = world.map.height * 16;
    if (arena_bg_valid && arena_bg_key == map_art.placed.data() && arena_bg_objects == map_art.placed.size() && arena_bg_w == w && arena_bg_h == h)
        return true;
    if (std::size_t(w) * h > 320u * 384u) { arena_bg.clear(); arena_bg.shrink_to_fit(); return false; }
    std::vector<u16> pal(map_art.palette.colors.size());
    for (std::size_t i = 0; i < pal.size(); ++i) pal[i] = u16(map_art.palette.colors[i] | 0x8000);
    arena_bg.assign(std::size_t(w) * h, u16(0x8000));
    for (int cy = 0; cy < world.map.height; ++cy)
        for (int cx = 0; cx < world.map.width; ++cx)
            if (const IndexedFrame* f = map_art.terrain_by_id[map_art.cells[std::size_t(cy) * map_art.width + cx]])
                blit_pal(arena_bg.data(), w, w, h, f->px.data(), f->w, f->h, cx * 16, cy * 16, pal.data(), int(pal.size()));
    for (const auto& o : map_art.placed)
        blit_pal(arena_bg.data(), w, w, h, o.frame->px.data(), o.frame->w, o.frame->h, o.left, o.top, pal.data(), int(pal.size()));
    arena_bg_w = w; arena_bg_h = h; arena_bg_key = map_art.placed.data(); arena_bg_objects = map_art.placed.size();
    arena_bg_valid = true;
    return true;
}

void draw_arena(int sx, int top) {
    const u32 t0 = cpuGetTiming();
    // terrain and objects: rows of the cached arena image, black outside the map
    if (build_arena_bg()) {
        for (int y = 0; y < kArenaH; ++y) {
            u16* d = back + y * kW;
            const int my = top + y;
            if (my < 0 || my >= arena_bg_h) { fill32(reinterpret_cast<u32*>(d), 0x80008000u, kW / 2); continue; }
            const int a = std::clamp(-sx, 0, kW), b = std::clamp(arena_bg_w - sx, a, kW);
            for (int x = 0; x < a; ++x) d[x] = 0x8000;
            const u16* from = arena_bg.data() + std::size_t(my) * arena_bg_w + (sx + a);
            if (b > a && !((reinterpret_cast<std::uintptr_t>(from) | a | (b - a)) & 1))
                copy32(reinterpret_cast<u32*>(d + a), reinterpret_cast<const u32*>(from), (b - a) / 2);
            else if (b > a) std::memcpy(d + a, from, std::size_t(b - a) * 2);
            for (int x = b; x < kW; ++x) d[x] = 0x8000;
        }
    } else {
        fill({0, 0, kW, kArenaH}, u16(RGB15(0, 0, 0) | 0x8000));
        std::vector<u16> pal(map_art.palette.colors.size());
        for (std::size_t i = 0; i < pal.size(); ++i) pal[i] = u16(map_art.palette.colors[i] | 0x8000);
        for (int cy = std::max(0, floor_div16(top)); cy <= std::min(world.map.height - 1, floor_div16(top + kArenaH - 1)); ++cy)
            for (int cx = std::max(0, floor_div16(sx)); cx <= std::min(world.map.width - 1, floor_div16(sx + kW - 1)); ++cx)
                if (const IndexedFrame* f = map_art.terrain_by_id[map_art.cells[std::size_t(cy) * map_art.width + cx]])
                    blit_pal(back, kW, kW, kArenaH, f->px.data(), f->w, f->h, cx * 16 - sx, cy * 16 - top, pal.data(), int(pal.size()));
        for (const auto& o : map_art.placed)
            blit_pal(back, kW, kW, kArenaH, o.frame->px.data(), o.frame->w, o.frame->h, o.left - sx, o.top - top, pal.data(), int(pal.size()));
    }
    prof_arena_bg += cpuGetTiming() - t0;
    struct ActorsTimer { u32 t = cpuGetTiming(); ~ActorsTimer() { prof_arena_actors += cpuGetTiming() - t; } } actors_timer;
    // drops lie on the ground
    for (const auto& d : combat_world.drops()) {
        if (d.ttl <= 16 && d.ttl % 3 == 0) continue;
        if (const auto* f = art_frame(combat_art, d.type == -1 ? 10 : 9))
            blit_arena(*f, d.x * 16 + 8 - f->w / 2 - sx, d.y * 16 + 8 - f->h - top, false);
    }
    // actors and effects, back to front
    struct Item { int owner, ax, ay; };
    std::vector<Item> items;
    if (world.hero) {
        const auto h = world.hero->state();
        const auto hit = combat_world.hero_hit_offset();
        items.push_back({0, h.x_pixels + 8 + hit.first, h.y_pixels + 8 + hit.second});
    }
    const auto& live = combat_world.enemies();
    for (std::size_t i = 0; i < live.size(); ++i) {
        const auto& e = live[i];
        if (e.state == combat::EnemyState::dead || e.hidden) continue;
        int ax = e.x_px + 8 + (e.width - 1) * 8, ay = e.y_px + 8 + e.draw_offset_y;
        if (e.shake == 3 || e.shake == 1) { ax += combat::kDx[std::size_t(e.shake_direction)] * 3; ay += combat::kDy[std::size_t(e.shake_direction)] * 3; }
        items.push_back({(e.boss != combat::BossKind::none ? 5000 : 1000) + int(i), ax, ay});
    }
    for (std::size_t i = 0; i < combat_world.battle_effects().size(); ++i) {
        const auto& e = combat_world.battle_effects()[i];
        if (e.target == -2) items.push_back({3000 + int(i), e.x_px, e.y_px});
    }
    for (std::size_t i = 0; i < combat_world.projectiles().size(); ++i) {
        const auto& p = combat_world.projectiles()[i];
        int x = p.x * 16 + 8, y = p.y * 16 + 9;
        if (p.duration == 2 && p.frame - 1 == 1) { x += combat::kDx[std::size_t(p.direction)] * 8; y += combat::kDy[std::size_t(p.direction)] * 8; }
        items.push_back({2000 + int(i), x, y});
    }
    if (const auto& g = combat_world.guardian(); g.state != 0 && g.id >= 0) items.push_back({4000, g.x * 16 + 8, g.y * 16 + 8});
    for (std::size_t i = 0; i < combat_world.effects().size(); ++i)
        items.push_back({4001 + int(i), combat_world.effects()[i].x_px + 8, combat_world.effects()[i].y_px + 9});
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.ay < b.ay; });
    for (const auto& item : items) {
        if (item.ay - top < -120 || item.ay - top > kArenaH + 100) continue;
        std::vector<Layer> layers;
        if (item.owner >= 5000) {
            const auto& e = live[std::size_t(item.owner - 5000)];
            if (const auto* frame = boss_frame(e))
                for (const auto& layer : *frame)
                    if (const auto* f = art_frame(character_art.frames, 0x20000 + (boss_texture_base(e.sprite) << 8) + layer.frame))
                        layers.push_back({f, layer.x, layer.y, layer.mirrored});
            for (const auto& layer : layers) blit_boss16(back, kW, kW, kArenaH, *layer.frame, item.ax - sx + layer.x, item.ay - top + layer.y, layer.flip);
            layers = attached_layers(item.owner - 5000);
        } else layers = entity_layers(item.owner);
        for (const auto& layer : layers) blit_arena(*layer.frame, item.ax - sx + layer.x, item.ay - top + layer.y, layer.flip, layer.rows);
    }
    // damage numbers and MISS over everything
    for (const auto& p : combat_world.popups()) {
        int px = p.x_px, py = p.y_px;
        if (p.target == -1 && world.hero) { px = world.hero->state().x_pixels + 8; py = world.hero->state().y_pixels + 8; }
        else if (p.target >= 0 && std::size_t(p.target) < live.size()) {
            const auto& owner = live[std::size_t(p.target)];
            if (owner.generation == p.generation && owner.state != combat::EnemyState::dead) {
                px = owner.x_px + 8 + (owner.width - 1) * 8; py = owner.y_px + 8 + owner.draw_offset_y;
            }
        }
        for (const auto& layer : number_layers(p)) blit_arena(*layer.frame, px - sx + layer.x, py - 30 - p.age * 4 - top + layer.y, false);
    }
}

void draw_boss_bar(int y) { // the 3DS enemy bar, moved next to the buttons
    const combat::Enemy* shown = combat_world.target_enemy();
    if (!shown)
        for (const auto& e : combat_world.enemies())
            if (e.boss != combat::BossKind::none && e.state != combat::EnemyState::dead) { shown = &e; break; }
    fill({3, y, kW - 6, 11}, plum);
    if (!shown || shown->stats < 0 || std::size_t(shown->stats) >= enemy_stats.size()) return;
    const auto& st = enemy_stats[std::size_t(shown->stats)];
    const std::string name = std::size_t(st.name_text) < text_bank.size() ? text_bank[std::size_t(st.name_text)] : "?";
    text(name, 6, y, ink);
    bar({120, y + 3, 90, 5}, shown->hp, st.hp, C(0xe83428), C(0xff8c5a));
    const std::string n = std::to_string(std::max(0, shown->hp));
    text(n, 250 - text_width(n), y, ink);
}

void draw();
void draw_arena_part() { // rows 0..130: the arena and the boss bar; the rest stays
    if (!dual_arena() || ev.dialogue || ev.choice || !ev.status.empty() || game_menu) { draw(); return; }
    draw_arena(arena_sx, arena_oy + 192);
    draw_boss_bar(kArenaH);
    const u32 t0 = cpuGetTiming();
    copy32(reinterpret_cast<u32*>(vram), reinterpret_cast<const u32*>(back), (kArenaH + 11) * kW / 2);
    prof_arena_copy += cpuGetTiming() - t0;
}

int full_draws = 0;
void draw() {
    ++full_draws;
    hits.clear();
    mini.shown = false;
    fill({0, 0, kW, kH}, plum);
    fill({0, 0, kW, 1}, bronze_dark); fill({0, kH - 1, kW, 1}, bronze_dark);
    fill({0, 0, 1, kH}, bronze_dark); fill({kW - 1, 0, 1, kH}, bronze_dark);
    fill({1, 1, kW - 2, 1}, bronze_mid); fill({1, kH - 2, kW - 2, 1}, bronze_mid);
    fill({1, 1, 1, kH - 2}, bronze_mid); fill({kW - 2, 1, 1, kH - 2}, bronze_mid);
    const bool talking = ev.dialogue || ev.choice;
    if (ending_running()) { fill({0, 0, kW, kH}, C(0)); copy32(reinterpret_cast<u32*>(vram), reinterpret_cast<const u32*>(back), kW * kH / 2); return; }
    if (screen_open() && ev.status.empty() && !talking) {
        fill({0, 0, kW, kH}, plum);
        if (shop_is_active()) draw_shop(); else draw_service();
        copy32(reinterpret_cast<u32*>(vram), reinterpret_cast<const u32*>(back), kW * kH / 2);
        return;
    }
    if (game_menu && ev.status.empty() && !talking) {
        fill({0, 0, kW, kH}, plum);
        draw_game_menu();
        copy32(reinterpret_cast<u32*>(vram), reinterpret_cast<const u32*>(back), kW * kH / 2);
        return;
    }
    if (dual_arena() && ev.status.empty() && !talking && !game_menu) {
        draw_arena(arena_sx, arena_oy + 192);
        draw_boss_bar(kArenaH);
        draw_hud_row();
        draw_buttons();
        u32* dst = reinterpret_cast<u32*>(vram); // CPU copy (DMA lost this picture on boss maps)
        const u32* src = reinterpret_cast<const u32*>(back);
        for (int i = 0; i < kW * kH / 2; ++i) dst[i] = src[i];
        return;
    }
    draw_status_line();
    if (!ev.status.empty()) {
        frame({3, kAreaTop - 2, kW - 6, kAreaH + 6}, black);
        text(ev.status, 11, kAreaTop + 8, ink, kW - 22);
    } else if (talking) draw_dialogue();
    else if (game_menu) {
        const Rect box{3, kAreaTop - 2, kW - 6, kAreaH + 6};
        frame(box, black);
        if (game_menu_confirm) {
            text("Return to the title?\nUnsaved progress is lost.", box.x + 10, box.y + 12, gold, box.w - 20);
            const char* labels[2] = {"Yes", "No"};
            for (int i = 0; i < 2; ++i) {
                const Rect r{box.x + 20 + i * 110, box.y + 60, 90, 24};
                frame(r, game_menu_selection == i ? selected : panel);
                text(labels[i], r.x + (r.w - text_width(labels[i])) / 2, r.y + 7, ink);
                hits.push_back({r, 800 + i});
            }
        } else {
            const char* labels[3] = {"Continue", "Save", "Title"};
            for (int i = 0; i < 3; ++i) {
                const Rect r{box.x + 40, box.y + 8 + i * 34, box.w - 80, 28};
                frame(r, game_menu_selection == i ? selected : panel);
                text(labels[i], r.x + (r.w - text_width(labels[i])) / 2, r.y + 9, ink);
                hits.push_back({r, 700 + i});
            }
        }
    }
    else if (world.map.kind <= 14) draw_minimap();
    else draw_location_plate();
    draw_hud_row();
    if (!ev.status.empty()) draw_key_buttons({{bank(3908, "Ok"), kOk}}, -1);
    else if (ev.choice) draw_key_buttons({{bank(3915, "Yes"), kYes}, {bank(3916, "No"), kNo}}, ev.choice_yes ? 0 : 1);
    else if (ev.dialogue && ev.item_message) draw_key_buttons({{bank(3908, "Ok"), kOk}}, -1);
    else if (ev.dialogue) draw_key_buttons({{"A " + bank(3911, "Next"), kNext}, {"B " + bank(3910, "Skip"), kSkip}}, -1);
    else if (game_menu) draw_key_buttons({{"A OK", kOk}, {"B " + bank(3910, "Back"), kSkip}}, -1);
    else draw_buttons();
    u32* dst = reinterpret_cast<u32*>(vram);
    const u32* src = reinterpret_cast<const u32*>(back);
    for (int i = 0; i < kW * kH / 2; ++i) dst[i] = src[i];
}

int guardian_buttons_key() { // the cooldown covers' heights (what the skill buttons show)
    const auto& guardians = ev.runtime ? ev.runtime->guardians() : game.guardians;
    if (guardians.equipped >= guardians.slots.size() || !guardians.slots[guardians.equipped]) return -1;
    const auto& g = *guardians.slots[guardians.equipped];
    int key = g.id;
    for (const int skill : {g.calm, g.wild}) {
        const int full = skill >= 0 ? combat::CombatWorld::guardian_cooldown_full(g.id, skill) : 0;
        const int left = skill >= 0 ? combat_world.guardian_cooldown(g.id, skill) : 0;
        key = key * 32 + (full > 0 && left > 0 ? 1 + 13 * left / full : 0);
    }
    return key;
}
void update_guardian_buttons() { // repaint the two guardian skill buttons
    if (ev.dialogue || ev.choice || !ev.status.empty() || game_menu) return;
    const auto saved_hits = hits;
    for (int i = 0; i < 2; ++i) fill(button_rect(i), plum);
    draw_buttons();
    hits = saved_hits;
    copy_to_vram(button_rect(0));
    copy_to_vram(button_rect(1));
}
void update_menu_button() { // the STAT plate changed: repaint just the MENU button
    if (ev.dialogue || ev.choice || !ev.status.empty() || game_menu) return;
    const auto saved_hits = hits;
    const Rect b = button_rect(2);
    fill(b, plum);
    draw_buttons();
    hits = saved_hits;
    copy_to_vram(b);
}

// A single message on the bottom screen (startup, errors).
void message(const std::string& s) {
    hits.clear();
    fill({0, 0, kW, kH}, plum);
    if (font.atlas.width) text(s, 8, 8, ink, kW - 16);
    DC_FlushRange(back, sizeof(back));
    dmaCopy(back, vram, sizeof(back));
}

int hit_at(int x, int y) {
    for (const auto& h : hits)
        if (x >= h.r.x && y >= h.r.y && x < h.r.x + h.r.w && y < h.r.y + h.r.h) return h.action;
    return 0;
}

void load_art(const GameArchive& archive) {
    const auto all = [&](const char* stem) {
        std::vector<std::size_t> ids;
        const auto atlas = load_sprite_atlas(archive, stem);
        for (std::size_t i = 0; i < atlas.frames.size(); ++i) ids.push_back(i);
        return ids;
    };
    load_color_frames(archive, "m/face", all("m/face"), 0x100, art);
    load_color_frames(archive, "img/icoitm", all("img/icoitm"), 0x200, art);
    load_color_frames(archive, "grd/grdico", all("grd/grdico"), 0x500, art);
    load_color_frames(archive, "sgui/gmico", all("sgui/gmico"), 0x700, menu_art);
    load_color_frames(archive, "sgui/shop", {0, 1, 2, 3, 4, 5, 6, 7}, 0x900, menu_art); // ce.r: tabs, worse / better
    load_color_frames(archive, "img/glb", {0, 1, 2, 3, 5, 6, 7, 8, 14}, 0x800, menu_art);
    for (int icon : {9, 10, 11, 12, 13, 15}) { // localized EXP/HP/LV/NEXT/?/SP labels
        const std::string path = "fr-FR/_img_glb__" + std::to_string(icon) + ".png";
        if (!archive.contains(path)) continue;
        const auto image = decode_indexed_png(archive.read_entry(path));
        ColorFrame frame{int(image.width), int(image.height), {}};
        frame.px.resize(std::size_t(frame.w) * frame.h);
        for (std::size_t i = 0; i < frame.px.size(); ++i)
            frame.px[i] = image.rgba[i * 4 + 3] < 128 ? 0 : u16(to_rgb15(&image.rgba[i * 4]) | 0x8000);
        menu_art.insert_or_assign(0x800 + icon, std::move(frame));
    }
    load_color_frames(archive, "img/etcui", {1, 3, 5, 6, 7, 8, 9, 10, 11}, 0, combat_art); // 11: guardian cooldown cover
    load_color_frames(archive, "img/glb", {7}, 0x100, combat_art);
    load_color_frames(archive, "img/keepst", {0, 1, 2, 3, 4, 5, 6, 7, 8}, 0x300, combat_art);
    load_color_frames(archive, "img/emoti", all("img/emoti"), 0x600, combat_art);
    for (int bank = 0; bank < 3; ++bank) {
        load_color_frames(archive, "img/atteff" + std::to_string(bank + 1), {0, 1, 2}, 0x100 + bank * 16, combat_art);
        death_eif[std::size_t(bank)] = load_eif(archive, "enm/die/" + std::to_string(bank));
    }
    load_color_frames(archive, "enm/die/bang", all("enm/die/bang"), 0x200, combat_art);
    lvup_eif = load_eif(archive, "char/lvup.eif");
    for (int i = 0; i < 6; ++i) {
        load_color_frames(archive, "grd/" + std::to_string(i), {0, 1}, kGuardianFigure + i * 2, combat_art);
        for (int slot = 0; slot < 3; ++slot) {
            const std::string path = i == 0 && slot == 0 ? "grd/spr/0_01.eif" : "grd/spr/" + std::to_string(i) + "_" + std::to_string(slot) + ".eif";
            if (archive.contains(path)) guardian_fx[std::size_t(i)].slots[std::size_t(slot)] = load_eif(archive, path);
        }
    }
    if (archive.contains("grd/spr/0_02.eif")) guardian_fx[0].extra = load_eif(archive, "grd/spr/0_02.eif");
    static const char* atlases[3]{"grd/fi", "grd/wa", "grd/gr"};
    for (int a = 0; a < 3; ++a) load_color_frames(archive, atlases[a], all(atlases[a]), kGuardianAtlas + a * 256, combat_art);
    load_color_frames(archive, "char/lvup", all("char/lvup"), 0x400, combat_art);
    if (archive.contains("fr-FR/_img_etcui__0.png")) { // ce.s: the LEVEL UP caption
        const auto image = decode_indexed_png(archive.read_entry("fr-FR/_img_etcui__0.png"));
        ColorFrame frame{int(image.width), int(image.height), {}};
        frame.px.resize(std::size_t(frame.w) * frame.h);
        for (std::size_t i = 0; i < frame.px.size(); ++i)
            frame.px[i] = image.rgba[i * 4 + 3] < 128 ? 0 : u16(to_rgb15(&image.rgba[i * 4]) | 0x8000);
        combat_art.insert_or_assign(0x3ff, std::move(frame));
    } else load_color_frames(archive, "img/etcui", {0}, 0x3ff, combat_art);
    if (archive.contains("fr-FR/_img_etcui__4.png")) { // ce.v: the STAT plate
        const auto image = decode_indexed_png(archive.read_entry("fr-FR/_img_etcui__4.png"));
        ColorFrame frame{int(image.width), int(image.height), {}};
        frame.px.resize(std::size_t(frame.w) * frame.h);
        for (std::size_t i = 0; i < frame.px.size(); ++i)
            frame.px[i] = image.rgba[i * 4 + 3] < 128 ? 0 : u16(to_rgb15(&image.rgba[i * 4]) | 0x8000);
        combat_art.insert_or_assign(0x3fe, std::move(frame));
    } else load_color_frames(archive, "img/etcui", {4}, 0x3fe - 4, combat_art);
}
} // namespace ui

// data_dir: heroeslore/ beside the running .nds (argv[0]). A JAR on the card wins over one linked into
// the program (heroes_lore_jar.nds, tools/build_nds.sh <out> <game.jar>; map.cpp reads it in place), as the
// 3DS .cia. Linked, not NitroFS: NitroFS needs argv[0], which not every DSi loader passes.
std::string find_jar(const std::string& data_dir, const std::string& nds_dir) {
    std::vector<std::string> candidates;
    if (!data_dir.empty()) candidates = {data_dir + "Heroes_Lore.jar", nds_dir + "Heroes_Lore.jar"};
    for (const char* path : {"sd:/heroeslore/Heroes_Lore.jar", "fat:/heroeslore/Heroes_Lore.jar", "/heroeslore/Heroes_Lore.jar",
                             "sd:/Heroes_Lore.jar", "fat:/Heroes_Lore.jar", "/Heroes_Lore.jar"})
        candidates.push_back(path);
    for (const auto& path : candidates)
        if (FILE* file = std::fopen(path.c_str(), "rb")) { std::fclose(file); return path; }
    if (&game_jar_size && game_jar_size) return "embedded:/Heroes_Lore.jar";
    return {};
}

void log_hero_art() {
    sample_memory();
    char text[160];
    std::snprintf(text, sizeof(text), "hero class %d: %u frames %u KB, heap used %u KB peak %u KB free %u KB",
                  hero_art.character_class, unsigned(hero_art.frames.size()), unsigned(hero_art.bytes / 1024),
                  unsigned(memory.used / 1024), unsigned(memory.peak / 1024), unsigned(memory.free / 1024));
    log_file(text);
}

int fps_min = 0, fps_sum = 0, fps_samples = 0;
void log_fps() {
    if (!fps_samples) return;
    char text[96];
    std::snprintf(text, sizeof(text), "map %d fps min %d avg %d over %d s", world.map_id, fps_min, fps_sum / fps_samples, fps_samples);
    log_file(text);
    fps_min = fps_sum = fps_samples = 0;
}


// ---------------------------------------------------------------------------------------------
// Saves (save_game.cpp format): hero<class>.hls per hero and autosave.hls, next to the JAR.

std::string save_dir;
std::string save_path_for(int hero) { return save_dir + "hero" + std::to_string(hero) + ".hls"; }
// settings.hlc as the other ports (PortSettings); written when the Options page changes something.
extern TitleMenu title;
extern bool autosave_enabled;
PortSettings current_settings() {
    return {title.game_speed, title.instant_speech, title.smooth_camera, title.language, title.heroes_unlocked, title.sound, title.soundfont,
            title.smooth_motion, title.wide_ui, title.autosave, title.modern_ui, title.rumble};
}
// DS Options volume (port): the Sound row is Off / 10..100 % (Off is the original Sound Off), the
// Music row (the SoundFont row elsewhere) and the Effects row (WAVs and MIDI jingles such as game
// over) 0..100 % of it. Kept in volume.cfg beside settings.hlc (two bytes in older builds).
int sound_volume = 10, music_volume = 10, effects_volume = 10;
void load_volume() {
    if (FILE* f = std::fopen((save_dir + "volume.cfg").c_str(), "rb")) {
        const int a = std::fgetc(f), b = std::fgetc(f), c = std::fgetc(f);
        std::fclose(f);
        if (a >= 1 && a <= 10 && b >= 0 && b <= 10) { sound_volume = a; music_volume = b; }
        if (c >= 0 && c <= 10) effects_volume = c;
    }
}
void save_volume() {
    static int written = -1;
    const int key = (std::max(sound_volume, 1) * 16 + music_volume) * 16 + effects_volume; // Off keeps the level for On
    if (key == written) return;
    if (FILE* f = std::fopen((save_dir + "volume.cfg").c_str(), "wb")) {
        std::fputc(std::max(sound_volume, 1), f); std::fputc(music_volume, f); std::fputc(effects_volume, f);
        if (std::fclose(f) == 0) written = key;
    }
}
// Left / Right on the Options rows; the two volume rows are handled here, the rest by TitleMenu.
void options_navigate(int direction) {
    const int id = title.page == TitlePage::options && (direction == 3 || direction == 4) ? title.option_id() : -1;
    const int delta = direction == 4 ? 1 : -1;
    if (id == 0) {
        if (!title.sound && delta > 0) { title.sound = true; sound_volume = std::max(sound_volume, 1); }
        else if (title.sound) { sound_volume = std::clamp(sound_volume + delta, 0, 10); if (!sound_volume) { title.sound = false; sound_volume = 1; } }
    } else if (id == 5) music_volume = std::clamp(music_volume + delta, 0, 10);
    else if (id == 10) effects_volume = std::clamp(effects_volume + delta, 0, 10);
    else title.navigate(direction);
}
void load_settings() {
    load_volume();
    const auto s = read_port_settings(save_dir + "settings.hlc");
    title.game_speed = s.game_speed; title.instant_speech = s.instant_speech; title.smooth_camera = s.smooth_camera;
    title.language = s.language; title.heroes_unlocked = s.heroes_unlocked; title.sound = s.sound; title.soundfont = s.soundfont;
    title.smooth_motion = s.smooth_motion; title.wide_ui = s.wide_ui; title.autosave = autosave_enabled = s.autosave; title.modern_ui = s.modern_ui; title.rumble = s.rumble;
}
void save_settings() {
    static PortSettings written{};
    static bool have = false;
    const auto now = current_settings();
    const auto same = [](const PortSettings& a, const PortSettings& b) {
        return a.game_speed == b.game_speed && a.instant_speech == b.instant_speech && a.smooth_camera == b.smooth_camera && a.language == b.language &&
               a.heroes_unlocked == b.heroes_unlocked && a.sound == b.sound && a.soundfont == b.soundfont && a.smooth_motion == b.smooth_motion &&
               a.wide_ui == b.wide_ui && a.autosave == b.autosave && a.modern_ui == b.modern_ui && a.rumble == b.rumble;
    };
    autosave_enabled = title.autosave;
    save_volume();
    if (have && same(now, written)) return;
    try { write_port_settings(save_dir + "settings.hlc", now); written = now; have = true; }
    catch (const std::exception& error) { log_file(std::string("settings: ") + error.what()); }
}
std::string autosave_path() { return save_dir + "autosave.hls"; }
bool file_exists(const std::string& path) {
    if (FILE* f = std::fopen(path.c_str(), "rb")) { std::fclose(f); return true; }
    return false;
}

SaveGame capture_save_raw();
SaveGame capture_save() {
    const auto hero = world.hero->state();
    if (ev.runtime || hero.motion_mode != 1 || hero.partial_x || hero.partial_y) throw std::runtime_error(tr("Finish the current action before saving.", "Сначала завершите текущее действие."));
    if (!map_allows_save(world.map_id)) throw std::runtime_error(tr("You cannot save during the boss stage.", "Во время боя с боссом сохраняться нельзя."));
    return capture_save_raw();
}
SaveGame capture_save_raw() {
    const auto hero = world.hero->state();
    SaveGame saved;
    saved.character_class = game.character_class; saved.map_id = world.map_id; saved.zone = std::stoi(game.zone);
    saved.x = hero.x_tile; saved.y = hero.y_tile; saved.direction = hero.direction; saved.quick_item_type = ui::quick_item_type;
    saved.switches = game.switches; saved.quests = game.quests; saved.inventory = game.inventory;
    saved.money = game.money; saved.equipment = game.equipment; saved.vitals = game.vitals;
    saved.progress = game.progress; saved.guardians = game.guardians;
    saved.recipes = game.recipes; saved.new_game_plus = game.new_game_plus; saved.clear_progress = game.clear_progress;
    return saved;
}

void restore_save(const GameArchive& archive, const SaveGame& saved) {
    log_fps();
    end_event();
    game = Persistent{};
    game.character_class = saved.character_class;
    game.zone = std::to_string(saved.zone);
    game.switches = saved.switches; game.quests = saved.quests; game.inventory = saved.inventory;
    game.money = saved.money; game.equipment = saved.equipment; game.vitals = saved.vitals;
    game.progress = saved.progress; game.guardians = saved.guardians;
    game.recipes = saved.recipes; game.new_game_plus = saved.new_game_plus; game.clear_progress = saved.clear_progress;
    try { // ae reads /enm/data<g>, g capped at 2
        enemy_stats = combat::parse_enemy_table(archive.read_entry("enm/data" + std::to_string(std::min(game.new_game_plus, 2))));
    } catch (const std::exception& error) { log_file(std::string("enemy table: ") + error.what()); }
    recalculate_vitals(game.progress, game.vitals);
    game.vitals.hp = std::min(game.vitals.hp, game.vitals.max_hp);
    game.vitals.sp = std::min(game.vitals.sp, game.vitals.max_sp);
    ui::quick_item_type = saved.quick_item_type;
    game_over = false;
    combat_world.reset_hero_state();
    load_hero_art(archive, saved.character_class);
    log_hero_art();
    if (!load_world(archive, saved.map_id, saved.x, saved.y, saved.direction)) throw std::runtime_error("The saved map could not be loaded.");
    world.last_auto_cell = std::size_t(saved.y) * world.map.width + saved.x; // no trigger under the loaded hero
    autosave_pending = false;
}

void begin_new_game(const GameArchive& archive, int character_class, const std::array<bool, 3>& choices) {
    const auto profile = make_new_game_profile(character_class);
    SaveGame fresh;
    fresh.character_class = profile.character_class; fresh.map_id = profile.map_id;
    fresh.zone = profile.character_class; fresh.x = profile.x; fresh.y = profile.y; fresh.direction = profile.direction;
    fresh.switches = profile.switches; fresh.quests = profile.quests; fresh.equipment = profile.equipment;
    fresh.money = profile.money;
    fresh.progress.attributes = profile.base_attributes;
    recalculate_vitals(fresh.progress, fresh.vitals);
    fresh.vitals.hp = fresh.vitals.max_hp; fresh.vitals.sp = fresh.vitals.max_sp;
    fresh.guardians = make_guardian_roster(choices);
    restore_save(archive, fresh);
    world.last_auto_cell.reset(); // the opening scene starts on the spawn tile
}


bool autosave_enabled = true; // Options Autosave (title.autosave)
void try_autosave() {
    if (!autosave_enabled) { autosave_pending = false; return; }
    if (!autosave_pending || ev.runtime || game_over || combat_world.hero_dying() || game.vitals.hp <= 0) return;
    if (!map_allows_save(world.map_id)) { autosave_pending = false; return; }
    const auto hero = world.hero->state();
    if (hero.motion_mode != 1 || hero.partial_x || hero.partial_y) return;
    autosave_pending = false;
    try { write_save(autosave_path(), capture_save()); }
    catch (const std::exception& error) { log_file(std::string("autosave failed: ") + error.what()); }
}

// ---------------------------------------------------------------------------------------------
// Title (title_menu.hpp logic; visual_ui.cpp layout): the logo with its falling feathers on the top
// screen (BG2, 16 bpp), the parchment menus of the original on the bottom screen.

TitleMenu title(true, false);
BitmapFont heading_font; // fonts/big.mf: capitals for menu headings, drawn without shadow
std::vector<std::string> hero_text, guardian_text, help_text;
std::vector<u16> top_buffer;      // 256x192 title picture, copied to BG2 during VBlank
bool top_dirty = false, title_shown = false;

void scan_saves() {
    title.saves.clear();
    if (file_exists(autosave_path()) || file_exists(autosave_path() + ".bak"))
        try {
            const auto saved = read_save(autosave_path());
            title.saves.push_back({saved.character_class, saved.progress.level, save_progress_percent(saved), true});
        } catch (const std::exception& error) { log_file(error.what()); }
    for (int hero = 6; hero <= 8; ++hero) {
        if (!file_exists(save_path_for(hero)) && !file_exists(save_path_for(hero) + ".bak")) continue;
        try {
            const auto saved = read_save(save_path_for(hero));
            title.saves.push_back({hero, saved.progress.level, save_progress_percent(saved)});
        } catch (const std::exception& error) { log_file(error.what()); }
    }
    title.continue_available = !title.saves.empty();
}

std::string bank_text(std::size_t i, const char* fallback) { return i < text_bank.size() && !text_bank[i].empty() ? text_bank[i] : fallback; }

void top_blit(const ColorFrame* f, int x, int y, bool flip = false) {
    if (!f) return;
    for (int yy = 0; yy < f->h; ++yy)
        for (int xx = 0; xx < f->w; ++xx) {
            const u16 v = f->px[std::size_t(yy) * f->w + (flip ? f->w - 1 - xx : xx)];
            const int px = x + xx, py = y + yy;
            if (v && px >= 0 && py >= 0 && px < 256 && py < 192) top_buffer[std::size_t(py) * 256 + px] = v;
        }
}

void draw_title_top() { // bs logo page: white, img/title1 2/3/4, feathers img/title2 (k), prompt
    top_buffer.assign(256 * 192, u16(0xffff));
    const int lx = 128 - 60, ly = 96 - 68;
    top_blit(art_frame(ui_art, 0x702), lx, ly + 25);
    top_blit(art_frame(ui_art, 0x703), lx + 52, ly + 25);
    top_blit(art_frame(ui_art, 0x704), lx + 93, ly + 2);
    for (int i = 0; i < 2; ++i) { // HCENTER|BOTTOM, the right one mirrored
        const auto& f = title.logo.feathers[std::size_t(i)];
        if (!f.initialised) continue;
        if (const auto* frame = art_frame(ui_art, 0x900 + TitleLogoAnimation::frame(f.phase)))
            top_blit(frame, f.x - frame->w / 2, f.y - frame->h, i == 1);
    }
    if (title.page == TitlePage::logo && title.logo.prompt_visible()) { // bh.q (3950) in black at h-45
        const std::string prompt = bank_text(3950, "PRESS ANY KEY");
        int w = 0;
        for (const auto& g : layout_bitmap_text(ui_font, prompt, 0, 0, 4096)) w = std::max(w, g.x + int(g.source_width));
        const auto& atlas = ui_font.atlas;
        for (const auto& g : layout_bitmap_text(ui_font, prompt, 0, 0, 4096))
            for (int yy = 0; yy < std::min<int>(ui_font.height, int(atlas.height)); ++yy)
                for (int xx = 0; xx < int(g.source_width); ++xx) {
                    const auto* p = &atlas.rgba[(std::size_t(yy) * atlas.width + g.source_x + xx) * 4];
                    if (p[3] < 128 || (p[0] < 64 && p[1] < 64 && p[2] < 64)) continue;
                    const int px = 128 - w / 2 + g.x + xx, py = 192 - 45 + g.y + yy;
                    if (px >= 0 && py >= 0 && px < 256 && py < 192) top_buffer[std::size_t(py) * 256 + px] = u16(0x8000);
                }
    }
    top_dirty = true;
}

void title_flush() { // during VBlank
    if (!top_dirty || bg_boss < 0) return;
    top_dirty = false;
    DC_FlushRange(top_buffer.data(), top_buffer.size() * 2);
    u16* vram = bgGetGfxPtr(bg_boss);
    for (int y = 0; y < 192; ++y) dmaCopy(top_buffer.data() + std::size_t(y) * 256, vram + y * 256, 256 * 2);
}

// as.b(Graphics): black screen, the hero's death pose at (cx, cy+20), "GAME OVER" (com 32) revealed
// two characters per draw at (cx - width/2, cy-20) with a grey shadow at +1.
void draw_game_over(int chars) {
    top_buffer.assign(256 * 192, u16(0x8000));
    for (std::uint8_t slot = 0; slot < 9; ++slot)
        for (std::size_t category = 0; category < hero_art.tables.size(); ++category) {
            const auto* record = find_character_sprite_record(hero_art.tables[category], slot, 2, 0);
            if (!record || record->frames.empty()) continue;
            for (const auto& layer : record->frames[0]) {
                if (layer.atlas_frame == 0xff) continue;
                if (const auto* f = art_frame(hero_art.frames, int(category << 8) | layer.atlas_frame))
                    top_blit(f, 128 + layer.x, 116 + layer.y, layer.mirrored && hero_art.mirror[category]);
            }
        }
    const std::string text = com(32, "GAME OVER");
    const auto glyphs = layout_bitmap_text(ui_font, text, 0, 0, 4096);
    int width = 0;
    for (const auto& g : glyphs) width = std::max(width, g.x + int(g.source_width));
    const auto& atlas = ui_font.atlas;
    for (int pass = 0; pass < 2; ++pass) {
        const u16 colour = pass == 0 ? u16(RGB15(15, 15, 15) | 0x8000) : u16(0xffff);
        int shown = 0;
        for (const auto& g : glyphs) {
            if (shown++ >= chars) break;
            for (int yy = 0; yy < std::min<int>(ui_font.height, int(atlas.height)); ++yy)
                for (int xx = 0; xx < int(g.source_width); ++xx) {
                    const auto* p = &atlas.rgba[(std::size_t(yy) * atlas.width + g.source_x + xx) * 4];
                    if (p[3] < 128 || (p[0] < 64 && p[1] < 64 && p[2] < 64)) continue;
                    const int px = 128 - width / 2 + g.x + xx + (pass == 0), py = 96 - 20 + g.y + yy + (pass == 0);
                    if (px >= 0 && py >= 0 && px < 256 && py < 192) top_buffer[std::size_t(py) * 256 + px] = colour;
                }
        }
    }
    top_dirty = true;
}

void show_title_screens(bool on) {
    if (on) {
        show_field(false);
        bgHide(bg_overlay);
        hide_caption();
        set_bg2_bitmap16(true);
        bgSetScroll(bg_boss, 0, 0);
        bgUpdate();
        u32* d = reinterpret_cast<u32*>(bgGetGfxPtr(bg_boss));
        for (int i = 0; i < 256 * 256 / 2; ++i) d[i] = 0xffffffffu;
        bgShow(bg_boss);
    } else {
        set_bg2_bitmap16(false);
        u32* d = reinterpret_cast<u32*>(bgGetGfxPtr(bg_boss));
        for (int i = 0; i < 256 * 256 / 2; ++i) d[i] = 0; // both 8 bpp pages
    }
    title_shown = on;
}

// ---------------------------------------------------------------------------------------------
// Game menu (ai): the original 155x176 box with six sgui/gmico tabs (Status q, Item ay, Equipment bz,
// Guardian bm/bn, Quest s, System d), centred on the bottom screen, touch keys under it. The rules
// follow main.cpp (confirm_menu / back_menu / navigate_menu).
std::vector<std::string> menu_text, attribute_text, quest_text; // sgui/gm, itm/itmatt, sgui/q<class>
int quest_text_class = -1;

struct GameMenu {
    int page = 0, status = 0, system = 0;
    std::size_t bag = 0, equip = 0, guardian = 0, quest = 0, recipe = 0;
    bool recipes_view = false; // port: the Quest tab shows the recipe journal (Y toggles)
    int item_action = 0, item_choice = 0; // ay/af: 4 Equip/Trash, 5 Use/Trash, 6 trash question
    std::string item_prompt, notice;
    int guardian_choice = -1;             // bm: Recall / Skill
    bool skill_screen = false, skill_calm = true;
    int skill_selection = 0;
    bool attr_edit = false, attr_confirm = false;
    int attr_selection = 0;
    std::array<int, 4> pending{};
    bool quest_reading = false, unequip_confirm = false, quit_confirm = false;
    bool tab_focus = false; // Modern: the cursor is in the tab column
    int detail_page = 0;
} gmenu;

std::string gm_text(int i) { return std::size_t(i) < menu_text.size() ? menu_text[std::size_t(i)] : std::string(); }
std::string com(int i) { return std::size_t(i) < com_text.size() ? com_text[std::size_t(i)] : std::string(); }
std::string catalog_text(const std::string& key) { // a decimal index into the language bank, or the text itself
    if (key.empty()) return key;
    std::size_t index = 0;
    for (const unsigned char ch : key) { if (ch < '0' || ch > '9') return key; index = index * 10 + (ch - '0'); }
    return index < text_bank.size() ? text_bank[index] : key;
}
std::string clean(std::string t) { t.erase(std::remove(t.begin(), t.end(), '|'), t.end()); return t; }

void refresh_gear() { // ao.n: gear bonuses, max HP/SP and the clamp of the current values
    game.progress.gear_bonus = gear_totals(game.equipment, item_catalog).bonus;
    recalculate_vitals(game.progress, game.vitals);
    game.vitals.hp = std::min(game.vitals.hp, game.vitals.max_hp);
    game.vitals.sp = std::min(game.vitals.sp, game.vitals.max_sp);
}

std::vector<QuestJournalEntry> quest_journal() {
    try { return build_quest_journal(game.character_class, quest_text, game.quests); }
    catch (const std::exception&) { return {}; }
}

struct RecipeRow { std::string result; int icon_type{-1}; std::vector<std::pair<std::string, int>> parts; };
std::string fold_case(const std::string& text) { // the lines write "root", the catalogue "Root"; Cyrillic folds too
    std::string out = text;
    for (std::size_t i = 0; i < out.size(); ++i) {
        auto& c = reinterpret_cast<unsigned char&>(out[i]);
        if (c < 0x80) { c = static_cast<unsigned char>(std::tolower(c)); continue; }
        if (c != 0xD0 || i + 1 >= out.size()) continue;
        auto& n = reinterpret_cast<unsigned char&>(out[i + 1]);
        if (n >= 0x90 && n <= 0x9F) n = static_cast<unsigned char>(n + 0x20);
        else if (n >= 0xA0 && n <= 0xAF) { c = 0xD1; n = static_cast<unsigned char>(n - 0x20); }
        else if (n == 0x81) { c = 0xD1; n = 0x91; }
        ++i;
    }
    return out;
}
std::string catalog_text(const std::string& key);
std::vector<RecipeRow> known_recipes() {
    std::vector<RecipeRow> list;
    for (std::size_t slot = 0; slot < kRecipeTexts.size(); ++slot) {
        const auto index = std::size_t(kRecipeTexts[slot]);
        if (!recipe_known(game.recipes, int(slot)) || index >= text_bank.size()) continue;
        const auto entry = parse_recipe_text(text_bank[index]);
        if (std::any_of(list.begin(), list.end(), [&](const RecipeRow& r) { return r.result == entry.result; })) continue;
        RecipeRow row; row.result = entry.result;
        for (const auto& item : item_catalog)
            if (fold_case(catalog_text(item.name_key)) == fold_case(entry.result)) { row.icon_type = int(item.type); break; }
        for (const auto& part : entry.parts) {
            int count = 0;
            for (const auto& stack : game.inventory)
                if (const auto* item = find_catalog_item(item_catalog, stack.item_type, stack.item_id); item && fold_case(catalog_text(item->name_key)) == fold_case(part))
                    count += stack.count;
            row.parts.push_back({part, count});
        }
        list.push_back(std::move(row));
    }
    return list;
}

struct MenuItem {
    int type{-1}, count{}, power{-1}, refine{}, refine_max{};
    bool weapon{}, identified{true};
    std::string name, description, option_name;
};
MenuItem present_stack(const InventoryStack& stack) {
    MenuItem row;
    row.type = stack.item_type; row.count = stack.count; row.identified = stack.identified; row.refine = stack.refine;
    const auto* item = find_catalog_item(item_catalog, stack.item_type, stack.item_id);
    if (!item) { row.name = "?"; return row; }
    row.name = clean(catalog_text(item->name_key));
    row.description = clean(catalog_text(item->description_key));
    if (const auto info = equipment_info(*item)) { row.power = int(info->power); row.weapon = item->type <= 2; row.refine_max = info->secondary; }
    const int option = stack.item_type <= 3 ? item_option(stack, item) : -1;
    if (option >= 0 && std::size_t(option) < attribute_text.size()) row.option_name = attribute_text[std::size_t(option)];
    std::string bonus;
    for (std::size_t i = 0; i < 4; ++i)
        if (stack.bonus[i] > 0 && 9 + i < hero_text.size()) bonus += hero_text[9 + i] + "+" + std::to_string(stack.bonus[i]) + "\n";
    row.description = bonus + row.description;
    return row;
}

namespace ui {
// Wide (port option): the box grows by ex; past the split column x moves right by ex, so the
// left column stays and the detail side widens; plain() draws unmapped.
int ex = 0, split = 1 << 20, kMenuX = 50;
int kMenuY = 0;
int wx(int x) { return x < split ? x : x + ex; }
struct Plain { int saved; Plain() : saved(split) { split = 1 << 20; } ~Plain() { split = saved; } };
constexpr std::uint32_t kWhite = 0xffffff, kTan = 0xdfbf7f;
constexpr int kTabHit = 900, kRowHit = 1000, kPageKey = 950, kSwitchKey = 951, kAttrHit = 960;
bool text_pages = false; // a detail text taller than its box: X pages it

void R(int x, int y, int w, int h, std::uint32_t c) {
    if (w <= 0 || h <= 0) return;
    const int x1 = wx(x);
    fill({kMenuX + x1, kMenuY + y, wx(x + w) - x1, h}, C(c));
}
void lines(int x, int y, int w, int h, std::uint32_t c1, std::uint32_t c2) { // cb.a (private)
    R(x + 1, y, w - 2, 1, c1); R(x, y + 1, 1, h - 2, c1); R(x + w - 1, y + 1, 1, h - 1, c2); R(x + 1, y + h - 1, w - 2, 1, c2);
}
void bevel(int x, int y, int w, int h, std::uint32_t c1, std::uint32_t c2, std::uint32_t c3, std::uint32_t inside) {
    R(x + 1, y, w - 2, 1, c1); R(x + w - 1, y + 1, 1, h - 2, c1); R(x + 1, y + h - 1, w - 2, 1, c1); R(x, y + 1, 1, h - 2, c1);
    R(x + 1, y + 1, w - 3, 1, c2); R(x + 1, y + 1, 1, h - 3, c2); R(x + w - 2, y + 1, 1, h - 3, c3); R(x + 1, y + h - 2, w - 2, 1, c3);
    R(x + 2, y + 2, w - 4, h - 4, inside);
}
void rounded(int x, int y, int w, int h, std::uint32_t c) {
    R(x + 1, y, w - 2, 1, c); R(x, y + 1, 1, h - 2, c); R(x + w - 1, y + 1, 1, h - 2, c); R(x + 1, y + h - 1, w - 2, 1, c); R(x + 1, y + 1, w - 2, h - 2, c);
}
const ColorFrame* glb(int n) { return art_frame(menu_art, 0x800 + n); }
// anchor: 20 top-left, 3 centre, 24 top-right, 36 bottom-left, 33 bottom-centre
void image(const ColorFrame* f, int x, int y, int anchor = 20) {
    if (!f) return;
    x = wx(x);
    if (anchor == 3) { x -= f->w / 2; y -= f->h / 2; } else if (anchor == 24) x -= f->w; else if (anchor == 36) y -= f->h;
    else if (anchor == 33) { x -= f->w / 2; y -= f->h; }
    blit(*f, kMenuX + x, kMenuY + y);
}
void say(const std::string& s, int x, int y, std::uint32_t c, int w = 150, int max_lines = 1000, int first = 0) {
    const int x1 = wx(x);
    text(clean(s), kMenuX + x1, kMenuY + y, C(c), wx(x + w) - x1, first, max_lines);
}
void say_paged(const std::string& s, int x, int y, std::uint32_t c, int w, int h) {
    const std::string t = clean(s);
    const int n = line_count(t, wx(x + w) - wx(x)), fits = std::max(1, h / fh());
    if (n <= fits) { say(t, x, y, c, w, fits); return; }
    text_pages = true;
    const int shown = std::max(1, fits - 1), pages = (n + shown - 1) / shown, page = gmenu.detail_page % pages;
    say(t, x, y, c, w, shown, page * shown);
    const std::string marker = std::to_string(page + 1) + "/" + std::to_string(pages);
    Plain unmapped;
    say(marker, wx(x + w) - text_width(marker), y + shown * fh(), kWhite, 40);
}
int digits(int value, int x, int y, bool right) { // r.c variant 0: 5x7 cells advancing 4 px (img/glb 5)
    const std::string t = std::to_string(std::max(0, value));
    const int w = int(t.size()) * 4;
    x = right ? wx(x) - w : wx(x);
    Plain unmapped;
    const ColorFrame* strip = glb(5);
    if (!strip) { say(t, x, y, kWhite, 60); return w; }
    for (std::size_t i = 0; i < t.size(); ++i)
        for (int yy = 0; yy < 7 && yy < strip->h; ++yy)
            for (int xx = 0; xx < 5; ++xx) {
                const int sx = (t[i] - '0') * 5 + xx;
                if (sx >= strip->w) continue;
                const int px = kMenuX + x + int(i) * 4 + xx, py = kMenuY + y + yy;
                if (const u16 v = strip->px[std::size_t(yy) * strip->w + sx]; v && px >= 0 && px < kW && py >= 0 && py < kH) back[py * kW + px] = v;
            }
    return w;
}
void ratio(int x, int y, int current, int maximum) { // r.d: current / maximum, right-aligned at x
    x = wx(x);
    Plain unmapped;
    const int w = digits(maximum, x, y, true);
    const ColorFrame* slash = glb(14);
    image(slash, x - w - (slash ? slash->w : 4), y);
    digits(current, x - w - 9, y, true);
}
void label(const std::string& s, int x, int y) { // r.a: white text on a black box
    const std::string t = clean(s);
    const int x1 = wx(x);
    Plain unmapped;
    R(x1 - 1, y - 1, text_width(t) + 2, fh() + 2, 0);
    say(t, x1, y, kWhite);
}
int bx = 2, by = 15; // tab content origin (Modern moves it into the detail panel)
void slot(int index, bool selected) { // cb.a(g,x,y,byte,boolean)
    const int x = bx + 3, y = by + 10 + index * 23;
    const std::uint32_t outer = selected ? 0x3f1f3f : 0x5f3f3f, light = selected ? 0x9f7f3f : 0xdfbf7f;
    R(x + 1, y, 24, 1, outer); R(x, y + 1, 1, 16, outer); R(x + 1, y + 17, 24, 1, outer);
    R(x + 1, y + 1, 24, 1, light); R(x + 1, y + 1, 1, 16, light);
    R(x + 2, y + 16, 23, 1, selected ? 0x3f3f3f : 0x7f5f7f);
    R(x + 2, y + 2, 24, 14, selected ? 0x5f3f3f : 0x9f7f7f);
}
void list(int count, int selection, bool arrows) { // cb.a(g,x,y,boolean): five slots, the detail panel beside them
    const int page = selection / 5, shown = std::clamp(count - page * 5, 0, 5);
    for (int i = 0; i < shown; ++i) {
        hits.push_back({{kMenuX + bx + 3, kMenuY + by + 10 + i * 23, wx(bx + 147) - (bx + 3), 18}, kRowHit + page * 5 + i});
        if (i != selection % 5) slot(i, false);
    }
    bevel(bx + 27, by + 10, 120, 137, 0x3f1f3f, 0x9f7f3f, 0x3f3f3f, 0x5f3f3f);
    if (shown > 0 || count == 0) slot(selection % 5, true);
    if (arrows) {
        Plain unmapped;
        if (page > 0) image(glb(0), bx + 70 + ex / 2, by + 4);
        if ((page + 1) * 5 < count) image(glb(1), bx + 70 + ex / 2, by + 148);
    }
}
void money() { // cb.a(g,x,y,int): digits, the coin to their left
    const int x = wx(bx + 110);
    Plain unmapped;
    const int w = digits(int(std::min<std::int64_t>(game.money, 999999999)), x, by + 2, true);
    image(glb(7), x - w, by + 2, 24);
}
std::string hero_label(int i) { return i >= 0 && std::size_t(i) < hero_text.size() ? hero_text[std::size_t(i)] : "?"; }

void item_detail(const MenuItem& item) { // cb.a(g, x, y, ad) at (33, 14)
    const int x = bx + 33; int y = by + 14;
    const int line = fh() + 2;
    if (item.power >= 0 && !item.identified) {
        say(item.name, x, y, kTan, 115, 4); y += (line_count(item.name, 115 + ex) - 1) * line;
        say(com(5), x, y + 16, kWhite, 115);
    } else if (item.power >= 0) {
        say(item.name, x, y, kWhite, 115, 4); y += (line_count(item.name, 115 + ex) - 1) * line;
        if (!item.option_name.empty()) say(item.option_name, x + 55, y + 10, 0xff0000, 60);
        say(com(item.weapon ? 4 : 46), x, y + 25, kTan, 80); digits(item.power, x + 108, y + 25, true);
        say(com(3), x, y + 40, kTan, 80); ratio(x + 108, y + 40, item.refine, item.refine_max);
        say_paged(item.description, x, y + 55, kTan, 110, std::max(10, by + 145 - (y + 55)));
    } else {
        say(item.name, x, y, kWhite, 115, 4); y += (line_count(item.name, 115 + ex) - 1) * line;
        say_paged(item.description, x, y + 15, kTan, 110, std::max(10, by + 145 - (y + 15)));
    }
}

void status_page_detail() { // q: one Status page at (bx+34, by+18)
    const auto& p = game.progress;
    if (gmenu.status == 0) {
        say(hero_label(game.character_class - 6), bx + 35, by + 18, kWhite);
        say(hero_label(game.character_class - 3), bx + 33, by + 35, kTan);
        image(glb(11), bx + 35, by + 52); digits(p.level, bx + 52, by + 52, false);
        image(glb(9), bx + 34, by + 70); digits(p.experience, bx + 102, by + 70, true);
        const int next = std::max(1, p.next_experience());
        R(bx + 34, by + 79, 72, 3, 0x3f1f3f);
        R(bx + 35, by + 80, 70 * std::min(p.experience, next) / next, 1, 0xffffff);
        image(glb(12), bx + 38, by + 84); digits(next, bx + 102, by + 84, true);
        image(glb(10), bx + 34, by + 97); ratio(bx + 102, by + 96, game.vitals.hp, game.vitals.max_hp);
        image(glb(15), bx + 34, by + 106); ratio(bx + 102, by + 105, game.vitals.sp, game.vitals.max_sp);
    } else if (gmenu.status == 1) {
        const auto gear = gear_totals(game.equipment, item_catalog);
        const int attack = combat::hero_base_attack(p.attributes[0] + gear.bonus[0]) + gear.weapon;
        const int defense = combat::hero_base_defense(p.attributes[0] + gear.bonus[0], p.level) + gear.defense;
        for (int i = 0; i < 6; ++i) {
            say(hero_label(9 + i), bx + 38, by + 21 + 15 * i, kTan, 60);
            digits(i < 4 ? p.attributes[std::size_t(i)] + p.gear_bonus[std::size_t(i)] : i == 4 ? attack : defense, bx + 100, by + 22 + 15 * i, true);
        }
    } else if (gmenu.status == 2) {
        say(hero_label(game.character_class - 6), bx + 34, by + 18, kTan);
        say_paged(hero_label(game.character_class), bx + 34, by + 30, kWhite, 110, 110);
    } else {
        rounded(bx + 34, by + 22, 100, 26, 0x3f1f3f);
        say(gm_text(3), bx + 37, by + 25, kWhite, 95); say(gm_text(4), bx + 37, by + 36, kWhite, 60);
        digits(p.attribute_points, bx + 99, by + 36, true);
        rounded(bx + 34, by + 62, 100, 33, 0x3f1f3f);
        say(gm_text(5), bx + 40, by + 72, kWhite, 20); say(gm_text(6), bx + 60, by + 67, kTan, 72); say(gm_text(7), bx + 60, by + 80, kTan, 72);
    }
}

// af: 80% of 240 wide, centred on the box; 1 message, 2 question, 3 list with the ce.d cursor.
void dialog(int type, const std::vector<std::string>& entries, int selection) {
    const int w = 192, tw = w - 10;
    const auto height_of = [&](const std::string& s) { return line_count(clean(s), tw) * fh(); };
    std::string joined;
    for (const auto& e : entries) if (!e.empty()) { if (!joined.empty()) joined += "\n"; joined += e; }
    int h = type == 2 ? 8 + height_of(joined) : 22 + height_of(joined);
    if (type == 3) { h = 12; for (const auto& e : entries) h += 3 + height_of(e); }
    Plain unmapped;
    const int x = 77 + ex / 2 - w / 2, y = 85 - h / 2;
    bevel(x, y, w, h, 0x1f1f3f, 0x5f3f3f, 0x1f1f3f, 0x3f1f3f);
    if (type == 3) {
        int ly = y + 7;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (int(i) == selection) image(glb(3), x + 5, ly);
            hits.push_back({{kMenuX + x + 2, kMenuY + ly - 2, w - 4, height_of(entries[i]) + 3}, kRowHit + int(i)});
            say(entries[i], x + 12, ly, kWhite, tw);
            ly += 3 + height_of(entries[i]);
        }
    } else say(joined, x + 5, y + 5, kWhite, tw);
}

// Touch keys under the box (the 3DS bottom-screen key row): A / B / X / Y with their words.
void menu_keys(const std::vector<std::pair<const char*, std::string>>& keys) {
    const int n = int(keys.size());
    if (!n) return;
    const int gap = 3, w = (kW - 6 - (n - 1) * gap) / n, y = 178, h = 14;
    for (int i = 0; i < n; ++i) {
        const Rect b{3 + i * (w + gap), y, w, h};
        frame(b);
        const std::string s = std::string(keys[std::size_t(i)].first) + " " + clean(keys[std::size_t(i)].second);
        text(s, b.x + std::max(3, (b.w - text_width(s)) / 2), b.y + (h - fh()) / 2, ink, b.w - 6, 0, 1);
        const std::string k = keys[std::size_t(i)].first;
        hits.push_back({{b.x, y - 1, b.w, h + 2}, k == "A" ? kOk : k == "B" ? kSkip : k == "X" ? kPageKey : kSwitchKey});
    }
}

void draw_game_menu() {
    if (modern_on() && !gmenu.attr_edit) { draw_menu_modern(); return; }
    text_pages = false;
    bx = 2; by = 15; kMenuY = 0;
    ex = title.wide_ui ? kW - 155 - 24 : 0; // the 3DS bottom screen's Wide rule (w - 155 - 24)
    kMenuX = (kW - 155 - ex) / 2;
    split = 1 << 20;
    const int tab = std::clamp(gmenu.page, 0, 5);
    R(0, 0, 155 + ex, 176, 0x3f1f3f);
    lines(2, 15, 151 + ex, 160, 0xffdfbf, 0xbf9f7f); R(3, 16, 149 + ex, 158, 0xdfbf7f);
    R(5 + tab * 16 + 1, 0, 14, 1, 0xffdfbf); R(5 + tab * 16, 1, 1, 16, 0xffdfbf);
    R(5 + tab * 16 + 15, 1, 1, 15, 0xbf9f7f); R(5 + tab * 16 + 1, 1, 14, 16, 0xdfbf7f);
    for (int i = 0; i < 6; ++i) {
        image(art_frame(menu_art, 0x700 + (i == 5 ? 6 : i)), 5 + 2 + 16 * i, 1);
        hits.push_back({{kMenuX + 5 + 16 * i, kMenuY, 16, 17}, kTabHit + i});
    }
    split = 100;
    const std::string back_word = bank(3909, "Back");
    std::vector<std::pair<const char*, std::string>> keys;
    if (tab == 0) {
        if (gmenu.attr_edit) {
            // The 3DS bottom screen's editor (bi sized for fingers): rows 30px, 22px -/+ buttons.
            Plain unmapped;
            const int pw = 143 + ex, px = 6, py = 20;
            R(px, py, pw, 152, 0x1f0f1f); R(px + 1, py + 1, pw - 2, 150, 0x5f3f3f);
            R(px + 4, py + 4, pw - 8, 20, 0x3f1f3f);
            say(clean(gm_text(36)) + " " + clean(gm_text(37)), px + 8, py + 4 + (20 - fh()) / 2, kTan, pw - 50);
            const std::string remaining = std::to_string(attribute_points_remaining(game.progress, gmenu.pending));
            say(remaining, px + pw - 10 - text_width(remaining), py + 4 + (20 - fh()) / 2, kWhite, 40);
            for (int i = 0; i < 4; ++i) {
                const int y = py + 28 + i * 30;
                const bool sel = gmenu.attr_selection == i;
                if (sel) R(px + 4, y, pw - 8, 28, 0x7f4f4f);
                say(hero_label(9 + i), px + 10, y + (28 - fh()) / 2, sel ? kWhite : kTan, pw - 90);
                const auto button = [&](int bx0, bool plus, int hit) {
                    R(bx0, y + 3, 22, 22, 0x1f0f1f); R(bx0 + 1, y + 4, 20, 20, 0xdfbf7f); R(bx0 + 2, y + 5, 18, 18, 0x3f1f3f);
                    R(bx0 + 6, y + 13, 10, 2, 0xffffff);
                    if (plus) R(bx0 + 10, y + 9, 2, 10, 0xffffff);
                    hits.push_back({{kMenuX + bx0 - 3, kMenuY + y, 28, 28}, hit});
                };
                const int minus_x = px + pw - 76, plus_x = px + pw - 28;
                button(minus_x, false, kAttrHit + i * 2);
                const int value = game.progress.attributes[std::size_t(i)] + game.progress.gear_bonus[std::size_t(i)] + gmenu.pending[std::size_t(i)];
                const std::string v = std::to_string(value);
                say(v, (minus_x + 22 + plus_x) / 2 - text_width(v) / 2, y + (28 - fh()) / 2, gmenu.pending[std::size_t(i)] > 0 ? 0xffdf7f : kWhite, 30);
                button(plus_x, true, kAttrHit + i * 2 + 1);
                hits.push_back({{kMenuX + px + 4, kMenuY + y, minus_x - px - 8, 28}, kRowHit + i});
            }
            keys = {{"A", bank(3913, "Select")}, {"B", back_word}};
        } else {
            label(gm_text(2), bx + 5, by); money();
            list(4, gmenu.status, false);
            for (int i = 0; i < 3; ++i) digits(i + 1, bx + 12, by + 16 + 23 * i, false);
            image(glb(8), bx + 10, by + 83);
            if (game.progress.attribute_points > 0) image(glb(6), bx + 3, by + 87, 36);
            status_page_detail();
            if (gmenu.status == 3 && game.progress.attribute_points > 0) keys = {{"A", gm_text(36)}, {"B", back_word}};
            else if (text_pages) keys = {{"X", tr("More", "Далее")}, {"B", back_word}};
            else keys = {{"B", back_word}};
        }
    } else if (tab == 1 || tab == 2) {
        std::vector<MenuItem> rows;
        if (tab == 1) for (const auto& s : game.inventory) rows.push_back(present_stack(s));
        else for (const auto& e : game.equipment) rows.push_back(e ? present_stack(*e) : MenuItem{});
        const std::size_t sel = tab == 1 ? gmenu.bag : gmenu.equip;
        label(tab == 1 ? com(2) : gm_text(20), bx + 5, by); money();
        list(tab == 1 ? int(rows.size()) : 5, int(sel), tab == 1);
        const std::size_t first = (sel / 5) * 5;
        for (std::size_t i = first; i < first + 5 && i < rows.size(); ++i) {
            const int row = int(i % 5);
            if (rows[i].type >= 0) {
                image(art_frame(art, 0x200 + rows[i].type), bx + 13, by + 18 + 23 * row + 1, 3);
                if (tab == 1 && rows[i].count > 1) digits(rows[i].count, bx + 13 + 11, by + 18 + 23 * row + 2, true);
            } else if (tab == 2) image(art_frame(menu_art, 0x707 + row), bx + 13, by + 19 + 23 * row, 3);
        }
        if (sel < rows.size() && rows[sel].type >= 0) item_detail(rows[sel]);
        else if (tab == 1) say(gm_text(15), bx + 33, by + 14, kWhite, 115);
        else if (sel == 1 && game.character_class != 8) say(gm_text(49), bx + 30, by + 14, kWhite, 115, 3);
        else say(gm_text(21), bx + 33, by + 14, kWhite, 115);
        if (tab == 1) keys = {{"A", clean(gm_text(13)) + "/" + clean(gm_text(11))}};
        else keys = {{"A", tr("Unequip", "Снять")}};
        if (text_pages) keys.push_back({"X", tr("More", "Далее")});
        keys.push_back({"B", back_word});
    } else if (tab == 3 && gmenu.skill_screen && game.guardians.slots[std::min<std::size_t>(4, gmenu.guardian)]) {
        // bn: key B / key Y tabs, three skills with icon, name, LEVEL, description.
        const auto& g = *game.guardians.slots[std::min<std::size_t>(4, gmenu.guardian)];
        const auto sk = [&](int i) { return std::size_t(i) < guardian_skill_names.size() ? guardian_skill_names[std::size_t(i)] : std::string(); };
        static constexpr int skill_level[]{1, 9, 20, 30};
        const int x = 2, y = 10;
        {
            Plain unmapped;
            bevel(x, y, 132, 22, 0x1f1f3f, 0x5f3f3f, 0x1f1f3f, 0x3f1f3f);
            const int t = x + (gmenu.skill_calm ? 0 : 66);
            R(t + 2, y + 2, 62, 19, 0x5f3f3f); R(t + 64, y + 1, 1, 19, 0x3f3f3f);
            R(t + 65, y + 1, 1, 17, 0x1f1f3f); R(t, y + 1, 1, 18, 0x1f1f3f); R(t + 1, y, 64, 1, 0x1f1f3f);
            R(t + 1, y + 2, 1, 18, 0x9f7f3f); R(t + 1, y + 1, 63, 1, 0x9f7f3f);
        }
        bevel(x, y + 19, 149, 140, 0x1f1f3f, 0x9f7f3f, 0x3f3f3f, 0x5f3f3f);
        const auto key = [&](int lx, int ly, const char* name, int skill) {
            Plain unmapped;
            const std::string s = clean(gm_text(38)) + " " + name;
            label(s, lx, ly);
            if (skill >= 0) image(art_frame(art, 0x500 + 6 + g.id * 4 + skill), lx + text_width(s) + 4, ly - 2);
        };
        key(x + 6, y + 5 + (gmenu.skill_calm ? 0 : 2), "B", g.calm);
        key(x + 71, y + 5 + (gmenu.skill_calm ? 2 : 0), "Y", g.wild);
        { Plain unmapped; image(glb(2), x, y + 7); image(glb(3), x + 130, y + 7); }
        rounded(x + 28, y + 26, 114, 127, 0x3f1f3f);
        rounded(x + 3, y + 31 + gmenu.skill_selection * 20, 26, 19, 0x3f1f3f);
        for (int k = 0; k < 3; ++k) image(art_frame(art, 0x500 + 6 + g.id * 4 + k), x + 5, y + 48 + k * 20, 36);
        for (int k = 0; k < 2; ++k) hits.push_back({{kMenuX + x + k * 66, kMenuY + y, 64, 20}, kAttrHit + 8 + k});
        for (int k = 0; k < 3; ++k) hits.push_back({{kMenuX + x + 1, kMenuY + y + 30 + k * 20, 28, 20}, kRowHit + k});
        say(sk(g.id * 8 + gmenu.skill_selection * 2), x + 34, y + 29, kWhite, 110);
        const std::string level = bank(3932, "LEVEL ");
        label(level, x + 34, y + 44);
        digits(skill_level[gmenu.skill_selection], x + 34 + text_width(clean(level)) + 5, y + 44, false);
        say(sk(g.id * 8 + gmenu.skill_selection * 2 + 1), x + 34, y + 53, kTan, 100, 95 / fh());
        keys = {{"A", bank(3913, "Select")}, {"B", back_word}};
    } else if (tab == 3) {
        // bm: grdico at (15, 19+23i); Equip mark, LV/EXP/NEXT block.
        label(gm_text(30), bx + 5, by);
        list(5, int(gmenu.guardian), false);
        for (std::size_t i = 0; i < game.guardians.slots.size() && i < 5; ++i)
            if (const auto& g = game.guardians.slots[i]) image(art_frame(art, 0x500 + g->id), bx + 15, by + 19 + int(i) * 23, 3);
        const auto& chosen = game.guardians.slots[std::min<std::size_t>(4, gmenu.guardian)];
        const auto gtext = [&](int i) { return std::size_t(i) < guardian_text.size() ? guardian_text[std::size_t(i)] : std::string("?"); };
        if (!chosen) say(gm_text(31), bx + 34, by + 18, kTan);
        else {
            const auto& g = *chosen;
            {
                Plain unmapped;
                label(gm_text(32), bx + 89 + ex, by + 22);
                if (gmenu.guardian == game.guardians.equipped) image(glb(6), bx + 100 + ex, by + 26, 36);
            }
            say(gtext(g.id), bx + 34, by + 18, kWhite, 55);
            say(gtext(g.id + 6), bx + 34, by + 35, kTan, 110);
            image(glb(11), bx + 34, by + 53); digits(g.level, bx + 50, by + 53, false);
            image(glb(9), bx + 34, by + 67); digits(g.experience, bx + 102, by + 67, true);
            const int next = std::max(1, g.next_experience());
            R(bx + 34, by + 76, 72, 3, 0x3f1f3f); R(bx + 35, by + 77, 70 * std::min(g.experience, next) / next, 1, 0xffffff);
            image(glb(12), bx + 38, by + 81); digits(next, bx + 102, by + 81, true);
        }
        keys = {{"A", bank(3913, "Select")}, {"B", back_word}};
    } else if (tab == 4 && gmenu.recipes_view) {
        // Port recipe journal in the s layout: result icons in the slots, the formula beside them.
        const auto recipes = known_recipes();
        label(tr("Recipes", "Рецепты"), bx + 5, by);
        if (recipes.empty()) {
            bevel(bx + 4, by + 10, 143, 137, 0x3f1f3f, 0x9f7f3f, 0x3f3f3f, 0x5f3f3f);
            say(tr("No recipes yet.", "Рецептов пока нет."), bx + 10, by + 15, kWhite, 96, 3);
        } else {
            gmenu.recipe = std::min(gmenu.recipe, recipes.size() - 1);
            list(int(recipes.size()), int(gmenu.recipe), true);
            const std::size_t first = (gmenu.recipe / 5) * 5;
            for (std::size_t i = first; i < first + 5 && i < recipes.size(); ++i)
                if (recipes[i].icon_type >= 0) image(art_frame(art, 0x200 + recipes[i].icon_type), bx + 13, by + 18 + 23 * int(i % 5) + 1, 3);
            const auto& r = recipes[gmenu.recipe];
            int y = by + 14;
            say(r.result, bx + 33, y, kWhite, 105, 2); y += std::min(2, line_count(clean(r.result), 105 + ex)) * 15;
            for (const auto& part : r.parts) {
                say("+ " + part.first, bx + 33, y, part.second > 0 ? kWhite : kTan, 90, 1);
                if (part.second > 0) digits(part.second, bx + 140, y + 1, true);
                y += 14;
            }
        }
        keys = {{"Y", tr("Quests", "Задания")}, {"B", back_word}};
    } else if (tab == 4) {
        // s: quest icon icoitm[18], name, Main/Sub, In progress, location and giver lines.
        const auto quests = quest_journal();
        label(gm_text(39), bx + 5, by);
        if (quests.empty()) {
            bevel(bx + 4, by + 10, 143, 137, 0x3f1f3f, 0x9f7f3f, 0x3f3f3f, 0x5f3f3f);
            say(gm_text(58), bx + 10, by + 15, kWhite, 96, 3);
            keys = {{"Y", tr("Recipes", "Рецепты")}, {"B", back_word}};
        } else {
            gmenu.quest = std::min(gmenu.quest, quests.size() - 1);
            list(int(quests.size()), int(gmenu.quest), true);
            const std::size_t first = (gmenu.quest / 5) * 5;
            for (std::size_t i = first; i < first + 5 && i < quests.size(); ++i) image(art_frame(art, 0x200 + 18), bx + 13, by + 18 + 23 * int(i % 5), 3);
            const auto& q = quests[gmenu.quest];
            int y = by + 14;
            say(q.name, bx + 33, y, kWhite, 105, 3); y += std::min(3, line_count(clean(q.name), 105)) * 15;
            say(gm_text(q.main_quest ? 55 : 56), bx + 33, y, kTan, 105); y += 15;
            say(gm_text(57), bx + 33, y, kTan, 105); y += 15;
            if (!q.location.empty()) { say(q.location, bx + 33, y, kWhite, 105, 2); y += 15; }
            if (!q.giver.empty()) say(q.giver, bx + 33, y, kWhite, 105, 2);
            if (gmenu.quest_reading) { // the port's reading view over the box
                bevel(-40, 8, 235, 160, 0x1f1f3f, 0x5f3f3f, 0x1f1f3f, 0x3f1f3f);
                say(q.name, -34, 14, 0xff9f00, 223, 2);
                say_paged(q.description + "\n\n" + clean(gm_text(54)) + "\n" + q.reward, -34, 14 + 2 * fh() + 4, kWhite, 223, 160 - 2 * fh() - 12);
                keys = {{"X", tr("Page", "Стр.")}, {"B", back_word}};
            } else keys = {{"A", tr("Read", "Читать")}, {"X", tr("Page", "Стр.")}, {"Y", tr("Recipes", "Рецепты")}, {"B", back_word}};
        }
    } else {
        // d: four 108px buttons, 22px apart (Save / Help / Options / Quit).
        label(gm_text(41), bx + 5, by);
        Plain unmapped;
        for (int i = 0; i < 4; ++i) {
            const bool sel = gmenu.system == i;
            const int x = 77 - 54 + ex / 2, y = by + 15 + 22 * i;
            hits.push_back({{kMenuX + x, kMenuY + y, 108, 19}, kRowHit + i});
            if (sel) bevel(x, y, 108, 19, 0x3f1f3f, 0x9f7f3f, 0x3f3f3f, 0x5f1f3f);
            else bevel(x, y, 108, 19, 0x5f3f3f, 0xdfbf7f, 0x7f5f7f, 0x9f7f7f);
            const std::string s = clean(gm_text(42 + i));
            say(s, x + 54 - text_width(s) / 2, y + 5, kWhite, 108);
        }
        keys = {{"A", bank(3913, "Select")}, {"B", back_word}};
    }
    menu_overlays(tab, keys);
}
// Dialogs over the box: they own the keys while open.
void menu_overlays(int tab, std::vector<std::pair<const char*, std::string>>& keys) {
    const std::string back_word = bank(3909, "Back");
    if (gmenu.quit_confirm) {
        dialog(2, {tr("Return to the title?", "Выйти в главное меню?"), tr("Unsaved progress is lost.", "Несохранённый прогресс пропадёт.")}, 0);
        keys = {{"A", bank(3915, "Yes")}, {"B", bank(3916, "No")}};
    } else if (gmenu.attr_confirm) {
        dialog(2, {gm_text(33)}, 0);
        keys = {{"A", bank(3915, "Yes")}, {"B", bank(3916, "No")}};
    } else if (tab == 3 && gmenu.guardian_choice >= 0) {
        dialog(3, {gm_text(22), gm_text(23)}, gmenu.guardian_choice);
        keys = {{"A", bank(3908, "Ok")}, {"B", back_word}};
    } else if (tab == 1 && (gmenu.item_action == 4 || gmenu.item_action == 5)) {
        dialog(3, {gm_text(gmenu.item_action == 4 ? 11 : 13), gm_text(10)}, gmenu.item_choice);
        keys = {{"A", bank(3908, "Ok")}, {"B", back_word}};
    } else if (tab == 1 && gmenu.item_action == 6) {
        dialog(2, {gmenu.item_prompt}, 0);
        keys = {{"A", bank(3908, "Ok")}, {"B", back_word}};
    } else if (tab == 2 && gmenu.unequip_confirm) {
        dialog(2, {tr("Take it off?", "Снять?")}, 0);
        keys = {{"A", bank(3915, "Yes")}, {"B", bank(3916, "No")}};
    } else if (!gmenu.notice.empty()) {
        dialog(1, {gmenu.notice}, 0);
        keys = {{"A", bank(3908, "Ok")}};
    }
    menu_keys(keys);
}
} // namespace ui

void open_game_menu(const GameArchive& archive) {
    game_menu = true;
    gmenu = GameMenu{};
    gmenu.tab_focus = ui::modern_on(); // Modern: the menu opens with the cursor in the tab column
    if (quest_text_class != game.character_class) {
        try { quest_text = parse_tdf_text(archive.read_entry("sgui/q" + std::to_string(game.character_class) + ".tdf"), text_bank); }
        catch (const std::exception&) { quest_text.clear(); }
        quest_text_class = game.character_class;
    }
    refresh_gear();
}

// The hero's art follows the worn pieces (equipment_visual_layers); sprite slots are recomposed.
void reload_hero_after_gear(const GameArchive& archive);

// confirm_menu / back_menu / navigate_menu of main.cpp for the DS keys and touch. Returns true when
// the player chose to quit to the title.
bool game_menu_input(const GameArchive& archive, u32 down, int action) {
    auto& m = gmenu;
    const auto gm = [](int i) { return gm_text(i); };
    const auto tabs_free = [&] {
        return m.notice.empty() && !m.attr_confirm && m.guardian_choice < 0 && !m.item_action && !m.skill_screen && !m.attr_edit &&
               !m.unequip_confirm && !m.quest_reading && !m.quit_confirm;
    };
    // Touch: tabs, rows (a tap on the selected row confirms), attribute arrows, key buttons.
    bool confirm = (down & KEY_A) || action == ui::kOk;
    bool back = (down & KEY_B) || action == ui::kSkip;
    int direction = (down & KEY_UP) ? 1 : (down & KEY_DOWN) ? 2 : (down & KEY_LEFT) ? 3 : (down & KEY_RIGHT) ? 4 : 0;
    if ((down & KEY_L) && tabs_free()) { m.page = (m.page + 5) % 6; m.detail_page = 0; return false; }
    if ((down & KEY_R) && tabs_free()) { m.page = (m.page + 1) % 6; m.detail_page = 0; return false; }
    if ((down & KEY_X) || action == ui::kPageKey) { ++m.detail_page; return false; }
    if (((down & KEY_Y) || action == ui::kSwitchKey) && m.page == 4 && !m.quest_reading) { m.recipes_view = !m.recipes_view; m.recipe = 0; return false; }
    if (down & KEY_START) { if (tabs_free()) { game_menu = false; return false; } back = true; }
    if (action >= ui::kTabHit && action < ui::kTabHit + 6) {
        if (tabs_free()) { m.page = action - ui::kTabHit; m.detail_page = 0; }
        return false;
    }
    if (action >= ui::kAttrHit && action < ui::kAttrHit + 8 && m.attr_edit) {
        m.attr_selection = (action - ui::kAttrHit) / 2;
        direction = (action - ui::kAttrHit) % 2 ? 4 : 3;
    }
    if (action >= ui::kAttrHit + 8 && action < ui::kAttrHit + 10 && m.skill_screen) {
        m.skill_calm = action == ui::kAttrHit + 8; m.notice.clear(); return false;
    }
    if (action >= ui::kRowHit) {
        const int row = action - ui::kRowHit;
        int* selection = nullptr;
        std::size_t* index = nullptr;
        if (m.guardian_choice >= 0) selection = &m.guardian_choice;
        else if (m.item_action == 4 || m.item_action == 5) selection = &m.item_choice;
        else if (m.skill_screen) selection = &m.skill_selection;
        else if (m.attr_edit) selection = &m.attr_selection;
        else if (m.page == 0) selection = &m.status;
        else if (m.page == 5) selection = &m.system;
        else if (m.page == 1) index = &m.bag;
        else if (m.page == 2) index = &m.equip;
        else if (m.page == 3) index = &m.guardian;
        else if (m.page == 4) index = m.recipes_view ? &m.recipe : &m.quest;
        m.tab_focus = false;
        if (selection) { if (*selection == row) confirm = true; else { *selection = row; m.detail_page = 0; m.notice.clear(); } }
        if (index) { if (*index == std::size_t(row)) confirm = true; else { *index = std::size_t(row); m.detail_page = 0; } }
        if (m.attr_edit && !confirm) return false;
    }
    if (back) {
        if (!m.notice.empty()) m.notice.clear();
        else if (m.quit_confirm) m.quit_confirm = false;
        else if (m.attr_confirm) m.attr_confirm = false;
        else if (m.guardian_choice >= 0) m.guardian_choice = -1;
        else if (m.item_action) m.item_action = 0;
        else if (m.skill_screen) m.skill_screen = false;
        else if (m.attr_edit) { m.attr_edit = false; m.pending.fill(0); }
        else if (m.quest_reading) m.quest_reading = false;
        else if (m.unequip_confirm) m.unequip_confirm = false;
        else game_menu = false;
        return false;
    }
    if (confirm) {
        if (!m.notice.empty()) { m.notice.clear(); return false; }
        if (m.tab_focus) { m.tab_focus = false; return false; } // Modern: A leaves the tab column
        if (m.quit_confirm) { m.quit_confirm = false; game_menu = false; return true; }
        if (m.page == 5) {
            if (m.system == 0) {
                try { write_save(save_path_for(game.character_class), capture_save()); m.notice = gm(46).empty() ? "Game saved" : gm(46); }
                catch (const std::exception& error) {
                    log_file(std::string("menu save failed: ") + error.what());
                    m.notice = (gm(47).empty() ? std::string("Failed.") : gm(47)) + "\n" + error.what();
                }
            } else if (m.system == 1 || m.system == 2) { // d: Help and Options open the title pages over the game
                title.active = true; title.in_game = true; title.message.clear(); title.text_page = 0;
                title.page = m.system == 1 ? TitlePage::help : TitlePage::options; title.selection = 0;
            } else m.quit_confirm = true;
            return false;
        }
        if (m.attr_edit) {
            const bool placed = m.pending != std::array<int, 4>{};
            if (!placed) m.notice = gm(34) + "\n" + gm(35);
            else if (!m.attr_confirm) m.attr_confirm = true;
            else {
                m.attr_confirm = false;
                if (apply_attribute_points(game.progress, game.vitals, m.pending)) { m.attr_edit = false; m.pending.fill(0); refresh_gear(); }
            }
            return false;
        }
        if (m.page == 0) {
            if (m.status == 3) {
                if (game.progress.attribute_points > 0) { m.pending.fill(0); m.attr_selection = 0; m.attr_edit = true; }
                else m.notice = gm(0);
            } else { m.status = (m.status + 1) % 4; m.detail_page = 0; }
            return false;
        }
        if (m.page == 2) { // port: A on a worn piece takes it off into the bag; the weapon stays
            const std::size_t slot = m.equip;
            if (slot >= game.equipment.size() || !game.equipment[slot]) return false;
            if (!m.unequip_confirm) {
                if (slot == 0) m.notice = tr("The weapon can only be swapped.", "Оружие можно только заменить.");
                else if (game.inventory.size() >= inventory_slot_limit) m.notice = ui::bank(3938, "Inventory is full.");
                else m.unequip_confirm = true;
                return false;
            }
            m.unequip_confirm = false;
            game.inventory.push_back(*game.equipment[slot]);
            game.equipment[slot].reset();
            reload_hero_after_gear(archive);
            return false;
        }
        if (m.page == 3) {
            auto& slot = game.guardians.slots[std::min<std::size_t>(4, m.guardian)];
            if (m.skill_screen && slot) {
                static constexpr int skill_level[]{1, 9, 20, 30};
                if (m.skill_selection == slot->calm || m.skill_selection == slot->wild) m.notice = gm(61);
                else if (skill_level[m.skill_selection] > slot->level) m.notice = gm(62) + "\n" + gm(63);
                else (m.skill_calm ? slot->calm : slot->wild) = m.skill_selection;
            } else if (m.guardian_choice >= 0 && slot) {
                if (m.guardian_choice == 0 && combat_world.guardian().state != 0) m.notice = gm(27) + "\n" + gm(28) + "\n" + gm(29);
                else if (m.guardian_choice == 0) {
                    game.guardians.equipped = std::min<std::size_t>(4, m.guardian);
                    m.notice = ui::bank(3933, "") + (std::size_t(slot->id) < guardian_text.size() ? guardian_text[std::size_t(slot->id)] : "");
                } else { m.skill_screen = true; m.skill_calm = true; m.skill_selection = 0; }
                m.guardian_choice = -1;
            } else if (slot) m.guardian_choice = 0;
            return false;
        }
        if (m.page == 4) {
            if (m.recipes_view) return false; // port: recipes have no reading view
            if (!quest_journal().empty()) { m.quest_reading = !m.quest_reading; m.detail_page = 0; }
            return false;
        }
        if (m.page == 1) {
            if (game.inventory.empty()) return false;
            m.bag = std::min(m.bag, game.inventory.size() - 1);
            const auto& stack = game.inventory[m.bag];
            const int type = stack.item_type;
            if (m.item_action == 0) { // ay.a(int,int): pick the af dialog for the selected item
                m.item_choice = 0; m.item_prompt.clear();
                if (equipment_slot(std::uint8_t(type)) >= 0) {
                    const int cls = game.character_class;
                    const bool own_weapon = (cls == 6 && type == 0) || (cls == 7 && type == 2) || (cls == 8 && type == 1);
                    if (!stack.identified) { m.item_action = 6; m.item_prompt = gm(12); }
                    else if (type <= 2 && !own_weapon) { m.item_action = 6; m.item_prompt = gm(type == 0 ? 8 : type == 2 ? 9 : 50) + "\n" + gm(12); }
                    else if (type == 3 && cls != 8) { m.item_action = 6; m.item_prompt = gm(12); }
                    else m.item_action = 4;
                } else if (is_quick_item(type)) m.item_action = 5;
                else if (type >= 18 && type <= 21) m.notice = gm(14);
                else { m.item_action = 6; m.item_prompt = gm(12); }
                return false;
            }
            const int act = m.item_action; m.item_action = 0;
            if ((act == 4 || act == 5) && m.item_choice == 1) { m.item_action = 6; m.item_prompt = gm(12); return false; }
            if (act == 6) { // g.a(slot, 1): trash one unit
                if (--game.inventory[m.bag].count <= 0) {
                    game.inventory.erase(game.inventory.begin() + std::ptrdiff_t(m.bag));
                    if (m.bag > 0 && m.bag >= game.inventory.size()) --m.bag;
                }
                return false;
            }
            if (act == 5) {
                bool antidote = false;
                if (use_consumable(game.inventory, m.bag, game.vitals, &antidote) == ItemUseResult::used) {
                    if (antidote) combat_world.cure_poison();
                    ui::quick_item_type = type;
                }
                if (!game.inventory.empty()) m.bag = std::min(m.bag, game.inventory.size() - 1);
                return false;
            }
            if (equip_bag_slot(game.character_class, m.bag, item_catalog, game.inventory, game.equipment) == EquipResult::equipped) {
                if (!game.inventory.empty()) m.bag = std::min(m.bag, game.inventory.size() - 1);
                reload_hero_after_gear(archive);
            }
            return false;
        }
        return false;
    }
    if (!direction) return false;
    if (!m.notice.empty() || m.attr_confirm || m.quit_confirm || m.unequip_confirm) return false;
    if (m.guardian_choice >= 0) { if (direction <= 2) m.guardian_choice ^= 1; return false; }
    if (m.item_action) { if ((m.item_action == 4 || m.item_action == 5) && direction <= 2) m.item_choice ^= 1; return false; }
    if (m.skill_screen) {
        if (direction >= 3) m.skill_calm = !m.skill_calm;
        else m.skill_selection = (m.skill_selection + (direction == 1 ? 2 : 1)) % 3;
        m.notice.clear(); return false;
    }
    if (m.attr_edit) {
        auto& p = m.pending[std::size_t(m.attr_selection)];
        if (direction <= 2) m.attr_selection = (m.attr_selection + (direction == 1 ? 3 : 1)) % 4;
        if (direction == 3 && p > 0) --p;
        if (direction == 4 && attribute_points_remaining(game.progress, m.pending) > 0) ++p;
        return false;
    }
    if (m.quest_reading) return false;
    if (ui::modern_on()) { // Modern: Left enters the tab column, Up/Down pick the tab there, Right returns
        if (direction >= 3) { m.tab_focus = direction == 3; return false; }
        if (m.tab_focus) { m.page = (m.page + (direction == 1 ? 5 : 1)) % 6; m.detail_page = 0; return false; }
    }
    if (direction >= 3) { m.page = (m.page + (direction == 3 ? 5 : 1)) % 6; m.detail_page = 0; return false; }
    const auto step = [&](std::size_t& v, std::size_t n) { if (n) v = (v + (direction == 1 ? n - 1 : 1)) % n; };
    m.detail_page = 0;
    if (m.page == 0) m.status = (m.status + (direction == 1 ? 3 : 1)) % 4;
    else if (m.page == 1) step(m.bag, game.inventory.size());
    else if (m.page == 2) step(m.equip, 5);
    else if (m.page == 3) step(m.guardian, 5);
    else if (m.page == 4 && m.recipes_view) step(m.recipe, known_recipes().size());
    else if (m.page == 4) step(m.quest, quest_journal().size());
    else m.system = (m.system + (direction == 1 ? 3 : 1)) % 4;
    return false;
}

void reload_hero_after_gear(const GameArchive& archive) {
    load_hero_art(archive, game.character_class);
    for (auto& slot : slots) { slot.owner = -1; slot.key = 0; }
    refresh_gear();
}

// ---------------------------------------------------------------------------------------------
// Trading post (bp/v/ab, sgui/shop) and the blacksmith / refiner (aa / ax, sgui/blak, sgui/refi,
// service.hpp), opened by an event's screen transition. Rules from main.cpp (refresh_shop,
// confirm_shop, navigate_shop, service_input); drawing from visual_ui (the 155x170 box).
std::vector<std::string> shop_ui_text, blacksmith_text, refiner_text;
std::vector<ShopItem> shop_catalog;
std::vector<MixRecipe> mix_table;
struct ShopState {
    bool active = false, selling = false, confirming = false, asking = false, tab_focus = false;
    int tab = 0, quantity = 1;
    std::size_t selection = 0;
    std::string notice, question;
    struct Row { const ShopItem* item; std::size_t slot; };
    std::vector<Row> rows;
} shop;
std::optional<ServiceSession> service;
u32 service_busy_until = 0; // vblank count when "Under refining." resolves
bool screen_open() { return shop.active || service.has_value(); }
bool shop_is_active() { return shop.active; }

std::string st(int i) { return std::size_t(i) < shop_ui_text.size() ? shop_ui_text[std::size_t(i)] : std::string(); }
ServiceInventory service_inventory() { return ServiceInventory{game.inventory, game.equipment, game.money, item_catalog}; }

MenuItem present_item(const ShopItem* item, int type) { // a catalogue entry (shop goods)
    MenuItem row;
    row.type = type;
    if (!item) { row.name = "?"; return row; }
    row.name = clean(catalog_text(item->name_key));
    row.description = clean(catalog_text(item->description_key));
    if (const auto info = equipment_info(*item)) { row.power = int(info->power); row.weapon = item->type <= 2; row.refine_max = info->secondary; }
    return row;
}
int shop_compare(const ShopItem& item) { // v: better / worse than the worn piece of that slot
    const int dest = equipment_slot(item.type);
    if (dest < 0 || !game.equipment[std::size_t(dest)]) return 0;
    const auto* old = find_catalog_item(item_catalog, game.equipment[std::size_t(dest)]->item_type, game.equipment[std::size_t(dest)]->item_id);
    const auto a = equipment_info(item);
    const auto b = old ? equipment_info(*old) : std::nullopt;
    if (!a || !b) return 0;
    const int delta = int(a->power) - int(b->power);
    return delta > 0 ? 1 : delta < 0 ? -1 : 0;
}

void refresh_shop() {
    gmenu.detail_page = 0;
    if (shop.selling && game.inventory.empty() && shop.notice.empty() && !shop.confirming) {
        // bb: once the last item is sold the list closes back to the shop with shop 21/22.
        shop.selling = false; shop.selection = 0;
        shop.notice = st(21) + "\n" + st(22);
    }
    shop.rows.clear();
    if (shop.selling) {
        for (std::size_t slot = 0; slot < game.inventory.size(); ++slot)
            shop.rows.push_back({find_catalog_item(item_catalog, game.inventory[slot].item_type, game.inventory[slot].item_id), slot});
    } else {
        for (const auto& item : shop_catalog) if (item.shop_category == shop.tab) shop.rows.push_back({&item, 0});
    }
    shop.selection = shop.rows.empty() ? 0 : std::min(shop.selection, shop.rows.size() - 1);
}
void open_shop(const GameArchive& archive) {
    try {
        if (shop_catalog.empty()) shop_catalog = load_shop_catalog(archive);
        shop = ShopState{};
        shop.active = true;
        refresh_shop();
    } catch (const std::exception& error) { ev.status = std::string("Shop load failed: ") + error.what(); }
}
void confirm_shop() {
    if (!shop.notice.empty()) { shop.notice.clear(); refresh_shop(); return; } // af type 1 acknowledged
    if (shop.rows.empty() || !shop.rows[shop.selection].item) return;
    const auto& row = shop.rows[shop.selection];
    if (shop.selling && !can_sell_item_type(row.item->type)) { shop.notice = st(19) + "\n" + st(20); refresh_shop(); return; }
    if (!shop.confirming) { shop.confirming = true; shop.quantity = 1; shop.notice.clear(); }
    else if (!shop.asking) {
        // ab: A asks once more; equipment for another hero adds "You can't equip it." (shop 26).
        shop.asking = true;
        shop.question = shop.selling ? st(23) : st(7);
        if (!shop.selling) {
            const int t = row.item->type, cls = game.character_class;
            const bool foreign = (t == 0 && cls != 6) || (t == 2 && cls != 7) || (t == 1 && cls != 8) || (t == 3 && cls != 8);
            if (foreign) shop.question = st(26) + "\n" + st(7);
        }
    } else {
        shop.confirming = shop.asking = false;
        if (shop.selling) {
            const auto result = sell_shop_item(*row.item, row.slot, shop.quantity, game.inventory, game.money);
            shop.notice = result == ShopSaleResult::sold ? clean(catalog_text(row.item->name_key)) + "\n" + st(24) : st(19) + "\n" + st(20);
            if (result == ShopSaleResult::sold) shop.selection = 0; // bb is rebuilt after a sale
        } else {
            switch (purchase_shop_item(*row.item, shop.quantity, game.inventory, game.money)) {
                case ShopPurchaseResult::purchased: shop.notice = st(11) + "\n" + st(12); break;
                case ShopPurchaseResult::insufficient_money: shop.notice = st(8); break;
                case ShopPurchaseResult::inventory_full: shop.notice = st(9) + "\n" + st(10); break;
                default: shop.notice = tr("Invalid purchase.", "Покупка невозможна."); break;
            }
        }
    }
    refresh_shop();
}
void navigate_shop(int direction) {
    if (!shop.notice.empty() || shop.asking) return;
    if (shop.confirming) {
        if (!shop.rows.empty() && shop.rows[shop.selection].item) {
            const auto& row = shop.rows[shop.selection];
            shop.quantity = step_shop_quantity(shop.quantity, direction, shop.selling, is_stackable_item(row.item->type),
                                               shop.selling ? game.inventory[row.slot].count : 0);
        }
        return;
    }
    if (!shop.selling && (direction == 3 || direction == 4)) { shop.tab = (shop.tab + (direction == 3 ? 5 : 1)) % 6; shop.selection = 0; }
    else if (!shop.rows.empty() && direction <= 2) {
        if (direction == 1) shop.selection = (shop.selection + shop.rows.size() - 1) % shop.rows.size();
        if (direction == 2) shop.selection = (shop.selection + 1) % shop.rows.size();
    }
    refresh_shop();
}
void toggle_shop_mode() {
    if (shop.confirming || shop.asking || !shop.notice.empty()) return;
    if (!shop.selling && game.inventory.empty()) { shop.notice = st(16) + "\n" + st(17); return; } // v: "No items to sell."
    shop.selling = !shop.selling;
    shop.selection = 0; shop.notice.clear();
    refresh_shop();
}
void back_shop() {
    if (!shop.notice.empty()) shop.notice.clear();
    else if (shop.asking) shop.asking = false;
    else if (shop.confirming) shop.confirming = false;
    else { shop.active = false; return; }
    refresh_shop();
}

void open_service(EventScreen screen) {
    service.emplace();
    service->kind = screen == EventScreen::refiner ? ServiceKind::refiner : ServiceKind::blacksmith;
    service_busy_until = 0;
}
void service_input(int direction, bool accept, bool cancel) {
    if (direction) service->navigate(direction);
    else if (accept) service->confirm(service_inventory(), mix_table, [](int lo, int hi) { return combat_world.rand_range(lo, hi); });
    else if (cancel) service->back();
    if (service->done) service.reset();
}
// at: "Under refining." stays up 500 ms (1500 ms on success) before the outcome; a destroyed device
// saves the game (n.o()).
void tick_service() {
    if (!service || !service->busy) return;
    if (!service_busy_until) { service_busy_until = vblanks + (service->success ? 90 : 30); return; }
    if (std::int32_t(vblanks - service_busy_until) < 0) return;
    service_busy_until = 0;
    service->finish_refine(service_inventory());
    if (service->autosave && map_allows_save(world.map_id))
        try { write_save(save_path_for(game.character_class), capture_save()); }
        catch (const std::exception& error) { log_file(std::string("autosave failed: ") + error.what()); }
    service->autosave = false;
    ui_dirty = true;
}

namespace ui {
constexpr int kFieldHit = 1100; // service widgets (v.cursor order)

void money_at(std::int64_t value, int x, int y) { // cb.a(g,x,y,int)
    x = wx(x);
    Plain unmapped;
    const int w = digits(int(std::clamp<std::int64_t>(value, 0, 999999999)), x, y, true);
    image(glb(7), x - w, y, 24);
}

void draw_shop() {
    if (modern_on()) { draw_shop_modern(); return; }
    text_pages = false;
    bx = 2; by = 15;
    ex = title.wide_ui ? kW - 155 - 24 : 0;
    kMenuX = (kW - 155 - ex) / 2; kMenuY = 4;
    split = 1 << 20;
    const int tab = std::clamp(shop.tab, 0, 5);
    R(0, 0, 155 + ex, 170, 0x3f1f3f);
    lines(2, 15, 151 + ex, 154, 0xffdfbf, 0xbf9f7f); R(3, 16, 149 + ex, 152, 0xdfbf7f);
    R(11 + tab * 16 + 1, 0, 14, 1, 0xffdfbf); R(11 + tab * 16, 1, 1, 16, 0xffdfbf);
    R(11 + tab * 16 + 15, 1, 1, 15, 0xbf9f7f); R(11 + tab * 16 + 1, 1, 14, 16, 0xdfbf7f);
    for (int i = 0; i < 6; ++i) {
        image(art_frame(menu_art, 0x900 + i), 11 + 2 + 16 * i, 1);
        hits.push_back({{kMenuX + 11 + 16 * i, kMenuY, 16, 17}, kTabHit + i});
    }
    image(glb(2), 4, 4); image(glb(3), 109, 4);
    split = 100;
    std::vector<std::pair<const char*, std::string>> keys;
    const auto& rows = shop.rows;
    if (shop.confirming && shop.selection < rows.size() && rows[shop.selection].item) {
        // ab: Balance box, question box, quantity arrows, item icon, total price.
        const auto& item = *rows[shop.selection].item;
        bevel(3, 20, 149, 29, 0x1f1f3f, 0x5f3f3f, 0x1f1f3f, 0x3f1f3f);
        bevel(3, 51, 149, 67, 0x1f1f3f, 0x5f3f3f, 0x1f1f3f, 0x3f1f3f);
        say(st(13), 26, 27, kTan, 80); money_at(game.money, 120, 31);
        const bool choose = shop.selling ? game.inventory[rows[shop.selection].slot].count > 1 : is_stackable_item(item.type);
        say(choose ? st(shop.selling ? 25 : 14) : st(15), 24, 58, kWhite, 125, 2);
        if (choose) {
            image(glb(2), 50, 85); digits(shop.quantity, 86, 85, true); image(glb(3), 95, 85);
            hits.push_back({{kMenuX + wx(40), kMenuY + 76, 24, 24}, kAttrHit});
            hits.push_back({{kMenuX + wx(88), kMenuY + 76, 24, 24}, kAttrHit + 1});
        }
        image(art_frame(art, 0x200 + item.type), 63, 77);
        money_at((shop.selling ? shop_sale_value(item, shop.quantity) : std::int64_t(item.price) * shop.quantity), 95, 105);
        keys = {{"A", shop.selling ? bank(3912, "Sell") : bank(3914, "Buy")}, {"B", bank(3909, "Back")}};
    } else {
        // v / bb: tab title (or "Select item to sell"), slots, better/worse marker, details.
        label(shop.selling ? st(18) : st(shop.tab + 1), 3, 15);
        const int saved_by = 15;
        (void)saved_by;
        list(int(rows.size()), int(shop.selection), true);
        const std::size_t first = (shop.selection / 5) * 5;
        for (std::size_t i = first; i < first + 5 && i < rows.size(); ++i) {
            const int type = rows[i].item ? int(rows[i].item->type) : shop.selling ? game.inventory[rows[i].slot].item_type : -1;
            if (type < 0) continue;
            image(art_frame(art, 0x200 + type), bx + 13, by + 18 + 23 * int(i % 5) + 1, 3);
            if (shop.selling && game.inventory[rows[i].slot].count > 1) digits(game.inventory[rows[i].slot].count, bx + 24, by + 18 + 23 * int(i % 5) + 2, true);
        }
        if (shop.selection < rows.size() && rows[shop.selection].item) {
            const auto& item = *rows[shop.selection].item;
            if (!shop.selling) {
                if (const int c = shop_compare(item)) image(art_frame(menu_art, c > 0 ? 0x907 : 0x906), bx + 20, by + 18 + int(shop.selection % 5) * 23, 33);
                item_detail(present_item(&item, item.type));
            } else item_detail(present_stack(game.inventory[rows[shop.selection].slot]));
        }
        keys = {{"A", bank(3913, "Select")}, {"Y", shop.selling ? bank(3914, "Buy") : bank(3912, "Sell")}};
        if (text_pages) keys.push_back({"X", tr("More", "Далее")});
        keys.push_back({"B", bank(3909, "Back")});
    }
    if (shop.asking) { dialog(2, {shop.question}, 0); keys = {{"A", bank(3915, "Yes")}, {"B", bank(3916, "No")}}; }
    else if (!shop.notice.empty()) { dialog(1, {shop.notice}, 0); keys = {{"A", bank(3908, "Ok")}}; }
    menu_keys(keys);
    kMenuY = 0;
}

void draw_service() {
    bx = 2; by = 15;
    const auto& v = *service;
    const auto inv = service_inventory();
    const auto& texts = v.kind == ServiceKind::blacksmith ? blacksmith_text : refiner_text;
    const auto t = [&](int i) { return std::size_t(i) < texts.size() ? texts[std::size_t(i)] : std::string(); };
    text_pages = false;
    ex = title.wide_ui ? kW - 155 - 24 : 0;
    kMenuX = (kW - 155 - ex) / 2; kMenuY = 4;
    split = 100;
    const auto panel = [&](int x, int y, int w, int h) { lines(x, y, w, h, 0xffdfbf, 0xbf9f7f); R(x + 1, y + 1, w - 2, h - 2, 0xdfbf7f); }; // cb.c
    const auto field = [&](int x, int y, int w, int h, bool selected) { // cb.a(g,x,y,w,h,boolean)
        if (selected) bevel(x, y, w, h, 0x3f1f3f, 0x9f7f3f, 0x3f3f3f, 0x5f1f3f);
        else bevel(x, y, w, h, 0x5f403f, 0xdfbf7f, 0x7f5f7f, 0x9f7f7f);
    };
    int field_index = 0;
    const auto button = [&](int y, const std::string& s, bool selected) { // cb.a(g,x,y,w,char[],boolean)
        const int w = 124, x = (155 - w) / 2;
        field(x, y, w, 19, selected);
        hits.push_back({{kMenuX + wx(x), kMenuY + y, wx(x + w) - wx(x), 19}, kFieldHit + field_index++});
        const int centre = (wx(x) + wx(x + w)) / 2;
        Plain unmapped;
        say(s, centre - text_width(clean(s)) / 2, y + 5, kWhite, w);
    };
    const auto item_box = [&](int x, int y, int ref, int number, const std::string& caption, bool selected) {
        // cb.a(g,x,y,ad,byte,char[],boolean): icon well, "Item n" label, caption, name field.
        rounded(x, y + 1, 28, 31, 0xbf9f7f);
        hits.push_back({{kMenuX + wx(x), kMenuY + y, wx(x + 147) - wx(x), 34}, kFieldHit + field_index++});
        const std::string name = clean(com(2));
        label(name, x + 2, y + 1); digits(number, x + 4 + text_width(name), y + 1, false);
        say(caption, x + 90, y + 2, kWhite, 60);
        field(x + 30, y + 14, 117, 19, selected);
        if (const auto* stack = service_item(inv, ref)) {
            image(art_frame(art, 0x200 + stack->item_type), x + 14, y + 19, 3);
            const auto row = present_stack(*stack);
            say(row.name, x + 34, y + 20, kWhite, 110, 1);
        }
    };
    if (v.page >= 0) {
        R(0, 0, 155, 170, 0x3f1f3f); panel(2, 4, 151, 162);
        if (v.kind == ServiceKind::blacksmith && v.page == 0) { // at
            label(t(13), 3, -2);
            rounded(3, 7, 149, 17, 0x9f7f7f); say(t(14), 6, 11, kWhite, 145, 1);
            item_box(4, 30, v.item, 1, t(15), v.cursor == 0);
            money_at(game.money, 147, 65);
            rounded(4, 73, 147, 38, 0x9f7f7f); say(t(16), 8, 80, kWhite, 100);
            if (service_item(inv, v.item)) { money_at(v.cost, 147, 80); say(clean(t(17)) + " " + std::to_string(v.need), 8, 93, kWhite, 140); }
            button(138, t(18), v.cursor == 1);
        } else if (v.kind == ServiceKind::blacksmith) { // ch
            label(t(13), 3, -2);
            rounded(3, 7, 149, 17, 0x9f7f7f); say(t(23), 6, 10, kWhite, 145, 1);
            item_box(4, 30, v.item, 1, t(15), v.cursor == 0);
            money_at(game.money, 145, 75);
            rounded(4, 83, 147, 20, 0x9f7f7f); say(t(24), 8, 88, kWhite, 100); money_at(100, 147, 89);
            button(138, t(25), v.cursor == 1);
        } else if (v.page == 0) { // ap
            label(t(14), 3, -2);
            item_box(4, 9, v.item, 1, t(15), v.cursor == 0);
            item_box(4, 45, v.orb, 2, t(16), v.cursor == 1);
            rounded(4, 81, 147, 31, 0xbf9f7f);
            const auto* orb = service_item(inv, v.orb);
            if (service_item(inv, v.item) && orb) {
                const std::string option = std::size_t(orb->item_id) < attribute_text.size() ? attribute_text[std::size_t(orb->item_id)] : std::string();
                int x = 6;
                for (const auto& part : {option, t(17), t(18)}) { say(part, x, 85, kWhite, std::max(10, 150 - x), 1); x += 2 + text_width(clean(part)); }
                money_at(500, 145, 86); money_at(game.money, 145, 101);
            }
            button(138, t(19), v.cursor == 2);
        } else { // k
            label(t(14), 3, -2);
            for (int i = 0; i < 3; ++i) item_box(4, 9 + 36 * i, v.mix[std::size_t(i)], i + 1, t(33), v.cursor == i);
            button(138, t(25), v.cursor == 3);
        }
    }
    if (v.picking) {
        // m: panel, title bar (3px lower for a single entry), slot list, selected item's details.
        const bool multi = v.picks.size() > 1;
        panel(2, -3, 151, 170);
        rounded(5, -2 + (multi ? 0 : 3), 145, 14, 0x9f7f7f); say(t(v.pick_title), 8, 1 + (multi ? 0 : 3), kWhite, 140, 1);
        // the list origin moves to (2, 11) here
        const int saved_y = kMenuY;
        kMenuY = saved_y - 4;
        list(int(v.picks.size()), v.pick_sel, multi);
        const std::size_t first = std::size_t(v.pick_sel / 5) * 5;
        for (std::size_t i = first; i < first + 5 && i < v.picks.size(); ++i)
            if (const auto* stack = service_item(inv, v.picks[i])) {
                image(art_frame(art, 0x200 + stack->item_type), bx + 13, by + 18 + 23 * int(i % 5) + 1, 3);
                if (stack->count > 1) digits(stack->count, bx + 24, by + 18 + 23 * int(i % 5) + 2, true);
            }
        if (std::size_t(v.pick_sel) < v.picks.size())
            if (const auto* stack = service_item(inv, v.picks[std::size_t(v.pick_sel)])) item_detail(present_stack(*stack));
        kMenuY = saved_y;
    }
    if (v.mix_confirm) { // bo: chosen materials, gold, "Mix cost" 500
        panel(10, 25, 135, 120);
        rounded(13, 28, 129, 17, 0x9f7f7f); say(t(25), 16, 29, kWhite, 125, 1);
        rounded(13, 50, 129, 60, 0x9f7f7f);
        int row = 0;
        for (int i = 0; i < 3; ++i)
            if (const auto* stack = service_item(inv, v.mix[std::size_t(i)])) say(present_stack(*stack).name, 16, 52 + 18 * row++, kWhite, 125, 1);
        money_at(game.money, 140, 115);
        rounded(13, 123, 129, 15, 0x9f7f7f); say(t(26), 16, 124, kWhite, 100, 1); money_at(500, 140, 130);
    }
    if (v.busy) { bevel(22, 74, 110, 22, 0x1f1f3f, 0x5f3f3f, 0x1f1f3f, 0x3f1f3f); say(t(28), 27, 79, kWhite, 100, 1); }
    std::string message;
    for (const int code : v.message) {
        const std::string s = code >= 0 ? t(code) : bank(std::size_t(-code), "");
        if (s.empty()) continue;
        if (!message.empty()) message += "\n";
        message += s;
    }
    std::vector<std::pair<const char*, std::string>> keys;
    if (!message.empty()) {
        dialog(v.question ? 2 : 1, {message}, 0);
        if (v.question) keys = {{"A", bank(3915, "Yes")}, {"B", bank(3916, "No")}};
        else keys = {{"A", bank(3908, "Ok")}};
    } else if (v.page < 0) {
        // af type 8: the question line, then the two services
        const int w = 192, tw = w - 10;
        const std::vector<std::string> entries = {t(0), t(1), t(2)};
        int h = 12;
        for (const auto& e : entries) h += 3 + line_count(clean(e), tw) * fh();
        Plain unmapped;
        const int x = 77 + ex / 2 - w / 2, y = 85 - h / 2;
        bevel(x, y, w, h, 0x1f1f3f, 0x5f3f3f, 0x1f1f3f, 0x3f1f3f);
        int ly = y + 7;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const int row = int(i) - 1, eh = line_count(clean(entries[i]), tw) * fh();
            if (row >= 0 && row == v.menu) image(glb(3), x + 5, ly);
            if (row >= 0) hits.push_back({{kMenuX + x + 2, kMenuY + ly - 2, w - 4, eh + 3}, kRowHit + row});
            say(entries[i], row < 0 ? x + 5 : x + 12, ly, kWhite, tw);
            ly += 3 + eh;
        }
        keys = {{"A", bank(3913, "Select")}, {"B", bank(3909, "Back")}};
    } else if (!v.busy) keys = {{"A", v.mix_confirm ? bank(3908, "Ok") : bank(3913, "Select")}, {"B", bank(3909, "Back")}};
    menu_keys(keys);
    kMenuY = 0;
}
} // namespace ui

// ---------------------------------------------------------------------------------------------
// Modern menus (port option Menus = Modern): one MW x MH frame with a tab column, named lists and a
// detail panel (visual_ui.cpp modern_*). On the DS: 240 x 176, icon-only tabs (26 px).
namespace ui {
constexpr int kMW = 240, kMH = 176, kMTop = 8, kMBody = kMH - 16, kTabsW = 26;
struct ModernRow { const ColorFrame* icon{}; std::string name; int count{}; bool mark{}; std::int64_t price{-1}; };

bool modern_on() { return title.modern_ui && title.wide_ui; }
void modern_frame() {
    ex = 0; split = 1 << 20; kMenuX = (kW - kMW) / 2; kMenuY = 0; bx = 2; by = 15;
    R(0, 0, kMW, kMH, 0x3f1f3f); lines(2, 2, kMW - 4, kMH - 4, 0xffdfbf, 0xbf9f7f); R(3, 3, kMW - 6, kMH - 6, 0xdfbf7f);
}
std::string fit(const std::string& s, int width) { // one line: a longer name loses whole characters and ends in ".."
    std::string t = clean(s);
    if (text_width(t) <= width) return t;
    while (!t.empty() && text_width(t + "..") > width) {
        do t.pop_back(); while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80);
        if (!t.empty() && static_cast<unsigned char>(t.back()) >= 0xC0) t.pop_back();
    }
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t + "..";
}
void modern_tabs(const std::vector<const ColorFrame*>& icons, int selected, bool focus) {
    Plain unmapped;
    for (int i = 0; i < int(icons.size()); ++i) {
        const int y = 8 + i * 22;
        hits.push_back({{kMenuX + 6, kMenuY + y, kTabsW - 6, 20}, kTabHit + i});
        if (i == selected) bevel(6, y, kTabsW - 6, 20, 0x3f1f3f, 0x9f7f3f, 0x3f3f3f, focus ? 0x3f1f3f : 0x5f3f3f);
        image(icons[std::size_t(i)], 9, y + 3);
    }
}
void modern_panel(int x, int y, int w, int h) { Plain unmapped; bevel(x, y, w, h, 0x3f1f3f, 0x9f7f3f, 0x3f3f3f, 0x5f3f3f); }
// Rows of icon, name, count or price; the window keeps the selection in view.
void modern_list(const std::vector<ModernRow>& rows, int selection, int lx, int top, int lw, int h, const std::string& empty, bool focus) {
    const int row_h = 19, visible = std::max(1, (h - 6) / row_h);
    const int count = int(rows.size()), sel = std::min(selection, std::max(0, count - 1));
    const int first = std::clamp(sel - visible / 2, 0, std::max(0, count - visible));
    modern_panel(lx, top, lw, h);
    Plain unmapped;
    if (rows.empty()) say(empty, lx + 6, top + 6, kWhite, lw - 12, 3);
    for (int i = first; i < count && i < first + visible; ++i) {
        const int y = top + 3 + (i - first) * row_h;
        const bool on = i == sel;
        const auto& row = rows[std::size_t(i)];
        hits.push_back({{kMenuX + lx + 3, kMenuY + y, lw - 6, row_h - 1}, kRowHit + i});
        if (on) rounded(lx + 3, y, lw - 6, row_h - 1, focus ? 0x7f5f5f : 0x3f1f3f);
        if (row.icon) image(row.icon, lx + 13, y + 9, 3);
        const int right = row.price >= 0 ? 52 : row.count > 1 ? 18 : row.mark ? 14 : 6;
        say(fit(row.name, lw - 25 - right), lx + 25, y + 5, on ? kWhite : kTan, lw - 25 - right + 2, 1);
        if (row.price >= 0) money_at(row.price, lx + lw - 6, y + 6);
        else if (row.count > 1) digits(row.count, lx + lw - 6, y + 6, true);
        if (row.mark) image(glb(6), lx + lw - 10, y + 13, 36);
    }
    if (first > 0) image(glb(0), lx + lw / 2 - 3, top - 5);
    if (first + visible < count) image(glb(1), lx + lw / 2 - 3, top + h - 2);
}
// The original detail code draws at (bx+ox, by+oy): put it inside the panel and fit it to dw.
void modern_detail(int dx, int dw, int ox, int oy) { ex = std::max(-50, dw - 125); bx = dx + 4 - ox; by = kMTop + 4 - oy; split = bx + 100; }

void menu_overlays(int tab, std::vector<std::pair<const char*, std::string>>& keys);

void draw_menu_modern() {
    text_pages = false;
    modern_frame();
    const int tab = std::clamp(gmenu.page, 0, 5);
    const bool focus = gmenu.tab_focus;
    modern_tabs({art_frame(menu_art, 0x700), art_frame(menu_art, 0x701), art_frame(menu_art, 0x702), art_frame(menu_art, 0x703),
                 art_frame(menu_art, 0x704), art_frame(menu_art, 0x706)}, tab, focus);
    const int lx = kTabsW + 6, lw = std::min(200, (kMW - lx - 10) * 11 / 20), dx = lx + lw + 4, dw = kMW - dx - 6;
    const std::string back_word = bank(3909, "Back");
    std::vector<std::pair<const char*, std::string>> keys;
    const auto gtext = [&](int i) { return std::size_t(i) < guardian_text.size() ? guardian_text[std::size_t(i)] : std::string("?"); };
    if (tab == 3 && gmenu.skill_screen && game.guardians.slots[std::min<std::size_t>(4, gmenu.guardian)]) {
        // bn with named rows: a bar with both keys and their skills, the three skills, the chosen one described.
        const auto& g = *game.guardians.slots[std::min<std::size_t>(4, gmenu.guardian)];
        const auto sk = [&](int i) { return std::size_t(i) < guardian_skill_names.size() ? guardian_skill_names[std::size_t(i)] : std::string(); };
        static constexpr int skill_level[]{1, 9, 20, 30};
        const int bar_h = 22, assigned = gmenu.skill_calm ? g.calm : g.wild;
        {
            Plain unmapped;
            for (int k = 0; k < 2; ++k) {
                const bool on = (k == 0) == gmenu.skill_calm;
                const int w = lw / 2, x = lx + k * w, skill = k == 0 ? g.calm : g.wild;
                bevel(x, kMTop, w, bar_h, 0x3f1f3f, 0x9f7f3f, 0x3f3f3f, on ? 0x3f1f3f : 0x7f5f5f);
                hits.push_back({{kMenuX + x, kMenuY + kMTop, w, bar_h}, kAttrHit + 8 + k});
                const std::string key = k == 0 ? "B" : "Y";
                say(key, x + 5, kMTop + 7, kWhite, 10, 1);
                const int cap = text_width(key) + 3;
                if (skill >= 0) {
                    image(art_frame(art, 0x500 + 6 + g.id * 4 + skill), x + 9 + cap, kMTop + 3);
                    say(fit(sk(g.id * 8 + skill * 2), w - cap - 32), x + 28 + cap, kMTop + 7, on ? kWhite : kTan, w - cap - 30, 1);
                }
            }
        }
        std::vector<ModernRow> rows;
        for (int k = 0; k < 3; ++k) rows.push_back({art_frame(art, 0x500 + 6 + g.id * 4 + k), sk(g.id * 8 + k * 2), 0, k == assigned});
        modern_list(rows, gmenu.skill_selection, lx, kMTop + bar_h + 2, lw, kMBody - bar_h - 2, std::string(), false);
        modern_panel(dx, kMTop, dw, kMBody);
        {
            Plain unmapped;
            say(sk(g.id * 8 + gmenu.skill_selection * 2), dx + 6, kMTop + 6, kWhite, dw - 12, 2);
            const std::string level = bank(3932, "LEVEL ");
            label(level, dx + 6, kMTop + 30);
            digits(skill_level[gmenu.skill_selection], dx + 6 + text_width(clean(level)) + 5, kMTop + 30, false);
            say_paged(sk(g.id * 8 + gmenu.skill_selection * 2 + 1), dx + 6, kMTop + 44, kTan, dw - 12, kMBody - 48);
        }
        keys = {{"A", bank(3913, "Select")}, {"B", back_word}};
        menu_overlays(tab, keys);
        return;
    }
    std::vector<ModernRow> rows;
    const bool recipe_list = tab == 4 && gmenu.recipes_view;
    std::vector<MenuItem> items;
    std::vector<QuestJournalEntry> quests;
    std::vector<RecipeRow> recipes;
    int selection = 0;
    if (tab == 1 || tab == 2) {
        if (tab == 1) for (const auto& st : game.inventory) items.push_back(present_stack(st));
        else for (const auto& e : game.equipment) items.push_back(e ? present_stack(*e) : MenuItem{});
        for (std::size_t i = 0; i < (tab == 1 ? items.size() : 5); ++i) {
            ModernRow row;
            if (i < items.size() && items[i].type >= 0) { row.icon = art_frame(art, 0x200 + items[i].type); row.name = items[i].name; if (tab == 1) row.count = items[i].count; }
            else { row.icon = art_frame(menu_art, 0x707 + int(i)); row.name = "-"; }
            rows.push_back(row);
        }
        selection = int(tab == 1 ? gmenu.bag : gmenu.equip);
    } else if (tab == 3) {
        for (std::size_t i = 0; i < 5; ++i) {
            ModernRow row;
            if (i < game.guardians.slots.size() && game.guardians.slots[i]) {
                row.icon = art_frame(art, 0x500 + game.guardians.slots[i]->id); row.name = gtext(game.guardians.slots[i]->id);
                row.mark = i == game.guardians.equipped;
            } else row.name = "-";
            rows.push_back(row);
        }
        selection = int(gmenu.guardian);
    } else if (recipe_list) {
        recipes = known_recipes();
        for (const auto& r : recipes) rows.push_back({r.icon_type >= 0 ? art_frame(art, 0x200 + r.icon_type) : nullptr, r.result});
        gmenu.recipe = recipes.empty() ? 0 : std::min(gmenu.recipe, recipes.size() - 1);
        selection = int(gmenu.recipe);
    } else if (tab == 4) {
        quests = quest_journal();
        for (const auto& q : quests) rows.push_back({art_frame(art, 0x200 + 18), q.name});
        gmenu.quest = quests.empty() ? 0 : std::min(gmenu.quest, quests.size() - 1);
        selection = int(gmenu.quest);
    } else if (tab == 0) { // q: the four Status pages; the bi icon marks unspent points
        for (const auto& name : {hero_label(game.character_class - 6), tr("Attributes", "Атрибуты"), tr("Profile", "Описание"), gm_text(3)})
            rows.push_back({nullptr, name});
        rows[3].icon = glb(8); rows[3].mark = game.progress.attribute_points > 0;
        selection = gmenu.status;
    } else {
        for (int i = 0; i < 4; ++i) rows.push_back({nullptr, gm_text(42 + i)}); // d: Save/Help/Options/Quit
        selection = gmenu.system;
    }
    if (recipe_list) { Plain unmapped; label(tr("Recipes", "Рецепты"), lx + 1, kMTop + 1); }
    modern_list(rows, selection, lx, recipe_list ? kMTop + 14 : kMTop, lw, recipe_list ? kMBody - 14 : kMBody,
                recipe_list ? tr("No recipes yet.", "Рецептов пока нет.") : gm_text(tab == 4 ? 58 : 15), focus);
    modern_panel(dx, kMTop, dw, kMBody);
    if (tab == 0) { modern_detail(dx, dw, 34, 18); status_page_detail(); }
    else if (tab == 5) { // port: who and where the save will record
        Plain unmapped;
        say(hero_label(game.character_class - 6), dx + 6, kMTop + 6, kWhite, dw - 12, 1);
        say(hero_label(game.character_class - 3), dx + 6, kMTop + 21, kTan, dw - 12, 1);
        image(glb(11), dx + 6, kMTop + 38); digits(game.progress.level, dx + 23, kMTop + 38, false);
        if (std::size_t(world.map_id) < location_names.size()) say(location_names[std::size_t(world.map_id)], dx + 6, kMTop + 54, kTan, dw - 12, 2);
    } else if (tab == 1 || tab == 2) {
        modern_detail(dx, dw, 33, 14);
        const std::size_t sel = std::size_t(selection);
        if (sel < items.size() && items[sel].type >= 0) item_detail(items[sel]);
        else if (tab == 1) say(gm_text(15), bx + 33, by + 14, kWhite, 115);
        else if (sel == 1 && game.character_class != 8) say(gm_text(49), bx + 33, by + 14, kWhite, 115, 3);
        else say(gm_text(21), bx + 33, by + 14, kWhite, 115);
    } else if (tab == 3) {
        modern_detail(dx, dw, 34, 18);
        const auto& chosen = game.guardians.slots[std::min<std::size_t>(4, gmenu.guardian)];
        if (!chosen) say(gm_text(31), bx + 34, by + 18, kTan);
        else {
            const auto& g = *chosen;
            say(gtext(g.id), bx + 34, by + 18, kWhite, 55, 1);
            say(gtext(g.id + 6), bx + 34, by + 35, kTan, 110, 1);
            image(glb(11), bx + 34, by + 53); digits(g.level, bx + 50, by + 53, false);
            image(glb(9), bx + 34, by + 67); digits(g.experience, bx + 102, by + 67, true);
            const int next = std::max(1, g.next_experience());
            R(bx + 34, by + 76, 72, 3, 0x3f1f3f); R(bx + 35, by + 77, 70 * std::min(g.experience, next) / next, 1, 0xffffff);
            image(glb(12), bx + 38, by + 81); digits(next, bx + 102, by + 81, true);
        }
    } else if (recipe_list) {
        if (!recipes.empty()) {
            Plain unmapped;
            const auto& r = recipes[gmenu.recipe];
            say(r.result, dx + 6, kMTop + 6, kWhite, dw - 12, 2);
            int y = kMTop + 36;
            for (const auto& part : r.parts) {
                say("+ " + fit(part.first, dw - 40), dx + 6, y, part.second > 0 ? kWhite : kTan, dw - 36, 1);
                if (part.second > 0) digits(part.second, dx + dw - 8, y + 1, true);
                y += 14;
            }
        }
    } else if (!quests.empty()) {
        modern_detail(dx, dw, 33, 14);
        const auto& q = quests[gmenu.quest];
        int y = by + 14;
        say(q.name, bx + 33, y, kWhite, 105, 3); y += std::min(3, line_count(clean(q.name), 105 + ex)) * 15;
        say(gm_text(q.main_quest ? 55 : 56), bx + 33, y, kTan, 105, 1); y += 15;
        say(gm_text(57), bx + 33, y, kTan, 105, 1); y += 15;
        if (!q.location.empty()) { say(q.location, bx + 33, y, kWhite, 105, 2); y += 15; }
        if (!q.giver.empty()) say(q.giver, bx + 33, y, kWhite, 105, 2);
    }
    // keys as the classic layout; the tab column is entered with Left
    if (tab == 0 && gmenu.status == 3 && game.progress.attribute_points > 0) keys = {{"A", gm_text(36)}};
    else if (tab == 1) keys = {{"A", clean(gm_text(13)) + "/" + clean(gm_text(11))}};
    else if (tab == 2) keys = {{"A", tr("Unequip", "Снять")}};
    else if (tab == 3 || tab == 5) keys = {{"A", bank(3913, "Select")}};
    else if (tab == 4 && !recipe_list) keys = {{"A", tr("Read", "Читать")}};
    if (tab == 4) keys.push_back({"Y", recipe_list ? tr("Quests", "Задания") : tr("Recipes", "Рецепты")});
    if (text_pages) keys.push_back({"X", tr("More", "Далее")});
    keys.push_back({"B", back_word});
    // the reading view and the dialogs centre on the original box
    ex = 0; split = 1 << 20; kMenuX = (kW - 155) / 2; bx = 2; by = 15;
    if (tab == 4 && gmenu.quest_reading && !quests.empty()) {
        const auto& q = quests[gmenu.quest];
        bevel(-40, 8, 235, 160, 0x1f1f3f, 0x5f3f3f, 0x1f1f3f, 0x3f1f3f);
        say(q.name, -34, 14, 0xff9f00, 223, 2);
        say_paged(q.description + "\n\n" + clean(gm_text(54)) + "\n" + q.reward, -34, 14 + 2 * fh() + 4, kWhite, 223, 160 - 2 * fh() - 12);
        keys = {{"X", tr("Page", "Стр.")}, {"B", back_word}};
    }
    menu_overlays(tab, keys);
}

// bp with named rows: categories on the left, goods with their price, the original details.
void draw_shop_modern() {
    text_pages = false;
    modern_frame();
    std::vector<const ColorFrame*> icons;
    for (int i = 0; i < 6; ++i) icons.push_back(art_frame(menu_art, 0x900 + i));
    modern_tabs(icons, shop.tab, shop.tab_focus);
    const int lx = kTabsW + 6, lw = std::min(210, (kMW - lx - 10) * 11 / 20), dx = lx + lw + 4, dw = kMW - dx - 6, list_top = kMTop + 14;
    { Plain unmapped; label(shop.selling ? st(18) : st(shop.tab + 1), lx + 1, kMTop + 1); }
    std::vector<ModernRow> rows;
    for (const auto& r : shop.rows) {
        ModernRow row;
        const int type = r.item ? int(r.item->type) : shop.selling ? game.inventory[r.slot].item_type : -1;
        row.name = shop.selling ? present_stack(game.inventory[r.slot]).name : r.item ? clean(catalog_text(r.item->name_key)) : "?";
        if (type >= 0) row.icon = art_frame(art, 0x200 + type);
        if (shop.selling) row.count = game.inventory[r.slot].count; else if (r.item) row.price = r.item->price;
        rows.push_back(row);
    }
    modern_list(rows, int(shop.selection), lx, list_top, lw, kMBody - 14, std::string(), shop.tab_focus);
    modern_panel(dx, kMTop, dw, kMBody);
    if (shop.confirming && shop.selection < shop.rows.size() && shop.rows[shop.selection].item) {
        // ab in the detail panel: the item, the question, quantity arrows, total and balance.
        const auto& row = shop.rows[shop.selection];
        const auto& item = *row.item;
        Plain unmapped;
        image(art_frame(art, 0x200 + item.type), dx + 14, kMTop + 14, 3);
        say(shop.selling ? present_stack(game.inventory[row.slot]).name : clean(catalog_text(item.name_key)), dx + 26, kMTop + 8, kWhite, dw - 30, 2);
        const bool choose = shop.selling ? game.inventory[row.slot].count > 1 : is_stackable_item(item.type);
        say(choose ? st(shop.selling ? 25 : 14) : st(15), dx + 6, kMTop + 36, kTan, dw - 12, 3);
        if (choose) {
            const int qy = kMTop + 80, cxq = dx + dw / 2;
            image(glb(2), cxq - 22, qy); digits(shop.quantity, cxq + 4, qy, true); image(glb(3), cxq + 12, qy);
            hits.push_back({{kMenuX + cxq - 34, kMenuY + qy - 8, 24, 24}, kAttrHit});
            hits.push_back({{kMenuX + cxq + 6, kMenuY + qy - 8, 24, 24}, kAttrHit + 1});
        }
        R(dx + 4, kMTop + 98, dw - 8, 1, 0x3f1f3f);
        say(shop.selling ? tr("Receive", "Получите") : tr("Total", "Итого"), dx + 6, kMTop + 104, kTan, dw - 12, 1);
        money_at(shop.selling ? shop_sale_value(item, shop.quantity) : std::int64_t(item.price) * shop.quantity, dx + dw - 6, kMTop + 118);
        say(st(13), dx + 6, kMTop + 132, kTan, dw - 12, 1);
        money_at(game.money, dx + dw - 6, kMTop + 146);
    } else if (shop.selection < shop.rows.size() && shop.rows[shop.selection].item) {
        modern_detail(dx, dw, 33, 14);
        const auto& item = *shop.rows[shop.selection].item;
        if (shop.selling) item_detail(present_stack(game.inventory[shop.rows[shop.selection].slot]));
        else {
            item_detail(present_item(&item, item.type));
            if (const int c = shop_compare(item)) { Plain unmapped; image(art_frame(menu_art, c > 0 ? 0x907 : 0x906), dx + dw - 14, kMTop + 24, 33); }
        }
    }
    std::vector<std::pair<const char*, std::string>> keys = {{"A", bank(3913, "Select")}, {"Y", shop.selling ? bank(3914, "Buy") : bank(3912, "Sell")}};
    if (shop.confirming) keys = {{"A", shop.selling ? bank(3912, "Sell") : bank(3914, "Buy")}};
    if (text_pages) keys.push_back({"X", tr("More", "Далее")});
    keys.push_back({"B", bank(3909, "Back")});
    ex = 0; split = 1 << 20; kMenuX = (kW - 155) / 2; kMenuY = 4; bx = 2; by = 15;
    if (shop.asking) { dialog(2, {shop.question}, 0); keys = {{"A", bank(3915, "Yes")}, {"B", bank(3916, "No")}}; }
    else if (!shop.notice.empty()) { dialog(1, {shop.notice}, 0); keys = {{"A", bank(3908, "Ok")}}; }
    menu_keys(keys);
    kMenuY = 0;
}
} // namespace ui

// Keys and touch for the open shop or service; true when it closed.
void screen_input(u32 down, int action) {
    const int direction = (down & KEY_UP) ? 1 : (down & KEY_DOWN) ? 2 : (down & KEY_LEFT) ? 3 : (down & KEY_RIGHT) ? 4 : 0;
    bool accept = (down & KEY_A) || action == ui::kOk, cancel = (down & KEY_B) || action == ui::kSkip;
    if (shop.active) {
        if ((down & KEY_X) || action == ui::kPageKey) { ++gmenu.detail_page; return; }
        if ((down & KEY_Y) || action == ui::kSwitchKey) { toggle_shop_mode(); return; }
        if (down & KEY_L) { navigate_shop(3); return; }
        if (down & KEY_R) { navigate_shop(4); return; }
        if (action >= ui::kTabHit && action < ui::kTabHit + 6) {
            if (!shop.selling && !shop.confirming && !shop.asking && shop.notice.empty()) { shop.tab = action - ui::kTabHit; shop.selection = 0; refresh_shop(); }
            return;
        }
        if (action == ui::kAttrHit || action == ui::kAttrHit + 1) { navigate_shop(action == ui::kAttrHit ? 3 : 4); return; }
        if (action >= ui::kRowHit && action < ui::kRowHit + 100 && !shop.confirming && !shop.asking && shop.notice.empty()) {
            const std::size_t row = std::size_t(action - ui::kRowHit);
            if (row == shop.selection) accept = true; else { shop.selection = row; gmenu.detail_page = 0; return; }
        }
        if (ui::modern_on() && !shop.selling && !shop.confirming && !shop.asking && shop.notice.empty()) {
            // Modern: the categories are a column; Left enters it, Up/Down pick there, Right or A returns.
            if (accept && shop.tab_focus) { shop.tab_focus = false; return; }
            if (direction >= 3) { shop.tab_focus = direction == 3; return; }
            if (direction && shop.tab_focus) { shop.tab = (shop.tab + (direction == 1 ? 5 : 1)) % 6; shop.selection = 0; refresh_shop(); return; }
        }
        if (accept) confirm_shop();
        else if (cancel) back_shop();
        else if (direction) navigate_shop(direction);
        return;
    }
    if (!service) return;
    auto& v = *service;
    if (action >= ui::kRowHit && action < ui::kRowHit + 100) { // af menu rows or picker slots
        const int row = action - ui::kRowHit;
        int& sel = v.picking ? v.pick_sel : v.menu;
        if (sel == row) accept = true; else { sel = row; return; }
    }
    if (action >= ui::kFieldHit && action < ui::kFieldHit + 8) {
        const int row = action - ui::kFieldHit;
        if (v.cursor == row) accept = true; else { v.cursor = row; return; }
    }
    service_input(direction, accept, cancel);
}

namespace ui {
constexpr u16 title_ink = C(0x3f1f3f), title_gold = C(0xff9f00), white = C(0xffffff), muted_red = C(0x9f7f7f);
constexpr int kMx = kW / 2 - 76, kMy = kH / 2 - 84; // visual_ui: mx = w/2-76, my = h/2-84

void art_at(int key, int x, int y, bool flip = false) { if (const auto* f = art_frame(art, key)) blit(*f, x, y, flip); }
void art_centred(int key, int cx, int cy) { if (const auto* f = art_frame(art, key)) blit(*f, cx - f->w / 2, cy - f->h / 2); }
void art_bottom_right(int key, int rx, int by) { if (const auto* f = art_frame(art, key)) blit(*f, rx - f->w, by - f->h); }

// bf.c / bf.b: 12 px corners, four 32 px spans, 24 px body rows; flat interior 0xffcb79.
void paper(Rect r) {
    const auto row = [&](int first, int y) {
        art_at(0x800 + first, r.x, y);
        for (int col = 0; col < 4; ++col) art_at(0x801 + first, r.x + 12 + 32 * col, y);
        art_at(0x802 + first, r.x + 140, y);
    };
    if (r.h == 24) { row(0, r.y); row(11, r.y + 12); return; }
    row(3, r.y); row(6, r.y + 12);
    const int rows = (r.h - 48) / 24;
    fill({r.x + 12, r.y + 36, 128, 24 * rows}, C(0xffcb79));
    for (int i = 0; i < rows; ++i) { art_at(0x800 + 9, r.x, r.y + 36 + 24 * i); art_at(0x800 + 10, r.x + 140, r.y + 36 + 24 * i); }
    row(11, r.y + 36 + 24 * rows);
}

// Heading font, capitals, centred on the panel. bh.a(int): black text gets the white outline (bh.d),
// white text the black one (bh.e).
void label(std::string s, int y, u16 colour) {
    if (text_language == 1) s = encode_russian_font_text(s); // back to the release's big-font codes
    else for (auto& c : s) if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    const u16 outline = (colour & 0x7fff) == (white & 0x7fff) ? C(0) : white;
    const BitmapFont& f = heading_font.atlas.width ? heading_font : ui_font;
    int w = 0;
    for (const auto& g : layout_bitmap_text(f, s, 0, 0, 4096)) w = std::max(w, g.x + int(g.source_width));
    const int x = kMx + (152 - w) / 2;
    for (const auto& g : layout_bitmap_text(f, s, 0, 0, 4096))
        for (int yy = 0; yy < std::min<int>(f.height, int(f.atlas.height)); ++yy)
            for (int xx = 0; xx < int(g.source_width); ++xx) {
                const auto* p = &f.atlas.rgba[(std::size_t(yy) * f.atlas.width + g.source_x + xx) * 4];
                if (p[3] < 128) continue;
                const bool fill = !(p[0] < 64 && p[1] < 64 && p[2] < 64);
                const int px = x + g.x + xx, py = y + g.y + yy;
                if (px >= 0 && py >= 0 && px < kW && py < kH) back[py * kW + px] = fill ? colour : outline;
            }
}


void af_box(const std::string& s) { // af: framed notice in the middle
    const int lines = 1 + int(std::count(s.begin(), s.end(), '\n'));
    const int w = 192, h = lines * fh() + 10, x = kW / 2 - w / 2, y = kH / 2 - h / 2;
    fill({x, y, w, h}, C(0x1f1f3f)); fill({x + 1, y + 1, w - 2, h - 2}, C(0x5f3f3f)); fill({x + 2, y + 2, w - 4, h - 4}, C(0x3f1f3f));
    text(s, x + 5, y + 5, white, w - 10);
}


void draw_title() {
    hits.clear();
    mini.shown = false;
    const auto& m = title;
    const auto X = [&](int o) { return kMx + o; };
    const auto Y = [&](int o) { return kMy + o; };
    if (m.page == TitlePage::logo) {
        fill({0, 0, kW, kH}, title_ink);
        const std::string prompt = tr("Touch or press any key", "Коснитесь экрана или нажмите кнопку");
        text(prompt, kW / 2 - text_width(prompt) / 2, kH / 2 - fh() / 2, title_gold);
        hits.push_back({{0, 0, kW, kH}, 1});
    } else if (m.page == TitlePage::quit_confirm) {
        fill({0, 0, kW, kH}, C(0));
        af_box(bank_text(3904, "Are You Sure?"));
        text("A " + bank_text(3915, "Yes"), 4, kH - fh() - 3, title_gold);
        text("B " + bank_text(3916, "No"), kW - 60, kH - fh() - 3, title_gold);
        hits.push_back({{0, kH / 2 - 40, kW / 2, 80}, 3});
        hits.push_back({{kW / 2, kH / 2 - 40, kW / 2, 80}, 600});
    } else if (m.page == TitlePage::guardian_intro) {
        fill({0, 0, kW, kH}, C(0));
        const Rect box{kW / 2 - 90, kH / 2 - 20, 180, 40};
        fill(box, title_ink);
        text(com(16, "GUARDIANS") + "\n" + com(13, "SELECTION."), box.x + 4, box.y + 4, title_gold, box.w - 8);
        text("A " + bank_text(3911, "Next"), 4, kH - fh() - 3, title_gold);
        hits.push_back({{0, 0, kW, kH}, 1});
    } else if (m.page == TitlePage::loading) {
        fill({0, 0, kW, kH}, C(0));
    } else {
        fill({0, 0, kW, kH}, title_ink);
        const bool main = m.page == TitlePage::menu;
        const std::string heading = m.page == TitlePage::options ? bank_text(3922, "Options") : m.page == TitlePage::help ? bank_text(3923, "Help")
                                    : m.page == TitlePage::reading ? (m.help_topic == 4 ? bank_text(3924, "About") : std::string(""))
                                    : m.page == TitlePage::load_game ? bank_text(3921, "Load Game") : bank_text(3920, "New Game");
        if (!main) { paper({kMx, kMy, 152, 24}); label(heading, kMy + 9, C(0)); }
        const int py = main ? kMy : kMy + 24;
        paper({kMx, py, 152, main ? 168 : m.page == TitlePage::load_game ? 120 : m.page == TitlePage::options ? 124 : 144});
        const auto yes_no = [&](int ox, int oy, int yes_x, int no_x, int brush_x) { // by/bk: etc[17] brush
            art_at(0x800 + 17, X(ox + brush_x + (m.yes ? 0 : 28)), Y(oy + 118));
            text(com(14, "YES"), X(ox + yes_x), Y(oy + 121), m.yes ? white : title_gold);
            text(com(15, "NO"), X(ox + no_x), Y(oy + 121), m.yes ? title_gold : white);
            hits.push_back({{X(ox + yes_x) - 8, Y(oy + 112), 34, 24}, 3});
            hits.push_back({{X(ox + no_x) - 8, Y(oy + 112), 34, 24}, 4});
        };
        if (main) { // bf.a: rows every 16 px from body+26, brush behind the selection
            const std::string actions[] = {bank_text(3920, "New Game"), bank_text(3921, "Load Game"), bank_text(3922, "Options"),
                                           bank_text(3923, "Help"), bank_text(3924, "About"), bank_text(3926, "Exit")};
            for (int i = 0; i < 6; ++i) {
                const int ry = py + 26 + i * 16;
                const int brush = m.brush_stage();
                if (m.selection == i)
                    if (const auto* f = art_frame(art, 0x800 + 14 + 2 * brush)) blit(*f, kMx + (111 - f->w) / 2, py + 24 + i * 16);
                label(actions[i], ry, m.selection == i && brush >= 2 ? white : C(0));
                if (i == 1 && !m.continue_available) text("--", kMx + 135, ry, title_ink);
                hits.push_back({{kMx + 10, ry - 3, 132, 16}, 100 + i});
            }
        } else if (m.page == TitlePage::character) { // c: grey faces, the selected one in colour
            const int selected = m.character_class - 6;
            static constexpr int slot_x[3]{6, 34, 59};
            for (int slot = 0; slot < 3; ++slot) {
                if (slot != 2 - selected) art_at(0xA10 + (2 - slot), X(15 + slot_x[slot]), Y(48));
                hits.push_back({{X(15 + slot_x[slot]), Y(48), 28, 48}, 200 + (2 - slot)});
            }
            art_at(0xA00 + selected, X(15 + slot_x[2 - selected]), Y(48));
            text(com(12, "CHARACTER"), X(26), Y(114), title_gold);
            text(com(13, "SELECTION."), X(26), Y(129), title_gold);
            if (m.locked_notice) af_box(com(6, "Unlock this character") + "\n" + com(7, "after doing Ronin's story"));
        } else if (m.page == TitlePage::character_confirm) {
            const int selected = m.character_class - 6;
            const auto hero_label = [&](std::size_t i) { return i < hero_text.size() ? hero_text[i] : std::string("?"); };
            text(hero_label(std::size_t(15 + selected)), X(21), Y(39), title_gold, 133);
            art_at(0x800 + 19, X(17), Y(85));
            text(hero_label(std::size_t(selected)), X(21), Y(89), title_gold);
            art_bottom_right(0xA00 + selected, X(135), Y(142));
            yes_no(10, 5, 9, 37, 5);
        } else if (m.page == TitlePage::guardians || m.page == TitlePage::guardian_confirm) {
            const bool confirming = m.page == TitlePage::guardian_confirm;
            const int ox = 15, oy = 10;
            art_at(0x800 + 19, X(ox + 11), Y(oy + 82));
            const int phase = int(vblanks / 3 % 6), bob = phase <= 3 ? phase : 6 - phase;
            for (int i = 0; i < 3; ++i) {
                const int cx = X(ox + 22 + i * 34);
                if (m.chosen_guardians[std::size_t(i)]) art_centred(0xA21 + i * 2, cx, Y(oy + 61));
                else art_centred(0xA20 + i * 2, cx, Y(oy + 54 + (!confirming && m.guardian_focus == i ? bob : 0)));
                if (!confirming) hits.push_back({{cx - 16, Y(oy + 34), 32, 44}, 300 + i});
            }
            if (!confirming) {
                art_at(0x800 + 20, X(ox + 19 + m.guardian_focus * 34), Y(oy + 73));
                if (guardian_text.size() >= 15) {
                    text(guardian_text[std::size_t(m.guardian_focus)], X(ox + 11), Y(oy + 94), title_gold);
                    text(guardian_text[std::size_t(12 + m.guardian_focus)], X(ox + 11), Y(oy + 109), title_gold, 100);
                }
            } else {
                text(com(17, "ARE YOU SURE?"), X(ox + 11), Y(oy + 104), title_gold);
                yes_no(ox, oy, 64, 92, 60);
            }
        } else if (m.page == TitlePage::load_game) {
            for (int i = 0; i < 5; ++i) art_at(0x800 + 19, X(23), Y(54 + i * 16));
            art_at(0x800 + 14 + 2 * m.brush_stage(), X(15), Y(36 + m.load_selection * 16));
            for (std::size_t i = 0; i < m.saves.size(); ++i) {
                const int hero = m.saves[i].hero - 6;
                std::string name = std::size_t(hero) < hero_text.size() ? hero_text[std::size_t(hero)] : "?";
                if (m.saves[i].autosave) name += " (auto)";
                text(name, X(31), Y(41 + int(i) * 16), int(i) == m.load_selection ? white : muted_red);
                hits.push_back({{X(15), Y(36 + int(i) * 16), 130, 16}, 400 + int(i)});
            }
            if (const auto* entry = m.selected_save()) {
                art_at(0xB0B, X(25), Y(109));
                const std::string level = std::to_string(std::max(0, entry->level));
                if (const auto* strip = art_frame(art, 0xB05)) // img/glb 5: 5x7 digits
                    for (std::size_t i = 0; i < level.size(); ++i) {
                        ColorFrame digit{5, std::min(7, strip->h), {}};
                        digit.px.resize(std::size_t(digit.w) * digit.h);
                        for (int y = 0; y < digit.h; ++y)
                            for (int x = 0; x < 5; ++x) {
                                const int sx = (level[i] - '0') * 5 + x;
                                digit.px[std::size_t(y) * 5 + x] = sx < strip->w ? strip->px[std::size_t(y) * strip->w + sx] : 0;
                            }
                        blit(digit, X(40) + int(i) * 4, Y(109));
                    }
                std::string progress = bank_text(3903, "Progress:");
                if (!progress.empty() && progress.back() != ':' && progress.back() != ' ') progress += ": ";
                text(progress + std::to_string(entry->progress) + "%", X(25), Y(122), C(0x7f5f5f));
                art_at(0xA00 + entry->hero - 6, X(93), Y(94));
            }
        } else if (m.page == TitlePage::overwrite_warning) {
            text(bank_text(3929, "Starting a new game will replace saved data."), X(20), Y(45), title_gold, 115);
            text("A " + bank_text(3911, "Next") + "   B " + bank_text(3910, "Back"), X(20), Y(130), title_ink);
        } else if (m.page == TitlePage::help) {
            static const char* fallback[] = {"Basic Controls", "Guardian", "Status", "Item"};
            for (int i = 0; i < 4; ++i) {
                label(std::size_t(i) < help_text.size() && !help_text[std::size_t(i)].empty() ? help_text[std::size_t(i)] : fallback[i], Y(51 + i * 15),
                      m.selection == i ? white : C(0));
                hits.push_back({{kMx + 10, Y(48 + i * 15), 132, 15}, 500 + i});
            }
        } else if (m.page == TitlePage::reading) {
            std::string body = bank_text(3905, "") + "\n\n" + bank_text(3906, "");
            if (m.help_topic < 4 && help_text.size() > std::size_t(m.help_topic + 6)) {
                body = help_text[std::size_t(m.help_topic + 6)];
                if (m.help_topic == 0) body = tr("DS: D-pad move, A talk/attack, X skill, B/Y guardian, L use item, R change item, START menu.\n\n",
                          "DS: крестовина - ход, A - разговор/атака, X - навык, B/Y - хранитель, L - предмет, R - смена предмета, START - меню.\n\n") + body;
            }
            const int visible = std::max(1, 104 / fh());
            const int lines = std::max(1, line_count(body, 130));
            const int pages = std::max(1, (lines + visible - 1) / visible), current = m.text_page % pages;
            text(std::to_string(current + 1) + "/" + std::to_string(pages), X(130), Y(34), title_ink);
            text(body, X(23), Y(45), title_gold, 130, current * visible, visible);
        } else if (m.page == TitlePage::options) {
            // be: labels at p2+27, values centred 70px further, rows 20px apart from p3+56, glb 2/3 arrows;
            // port: a tab strip (selection 0) above them, each tab holding 2-3 settings.
            static const char* speed[] = {"Slow", "Normal", "Fast"};
            const auto trim_word = [](std::string t) { while (!t.empty() && t.back() == ' ') t.pop_back(); return t; };
            const auto on_off = [&](bool on) { return trim_word(on ? bank_text(3944, "On") : bank_text(3945, "Off")); };
            const std::string names[] = {tr("Master", "Общая"), com(19, "Game"), com(20, "Speech"), com(21, "Camera"), tr("Language", "Язык"),
                                         tr("Music", "Музыка"), "60 FPS", tr("Menus", "Меню"), tr("Autosave", "Автосейв"), tr("Rumble", "Вибрация"),
                                         tr("Effects", "Звуки")};
            const std::string tabs[] = {tr("Sound", "Звук"), tr("Display", "Экран"), tr("Interface", "Интерфейс"), tr("Game", "Игра")};
            const ColorFrame* left = art_frame(menu_art, 0x802);
            const ColorFrame* right = art_frame(menu_art, 0x803);
            const std::string& t = tabs[std::clamp(m.options_tab, 0, 3)];
            text(t, X(76) - text_width(t) / 2, Y(41), m.selection == 0 ? C(0xffffff) : title_gold);
            if (left) blit(*left, X(28), Y(41));
            if (right) blit(*right, X(123), Y(41));
            hits.push_back({{X(20), Y(37), 30, 16}, 650});
            hits.push_back({{X(115), Y(37), 30, 16}, 651});
            for (int i = 0; i < TitleMenu::option_tab_count; ++i)
                fill({X(76 - (TitleMenu::option_tab_count * 6) / 2 + i * 6 + 1), Y(53), 3, 3}, i == m.options_tab ? C(0x7f3f3f) : C(0xbf8f5f));
            for (int row = 0; row < TitleMenu::option_rows(m.options_tab); ++row) {
                const int id = TitleMenu::option_ids[std::size_t(m.options_tab)][std::size_t(row)];
                const int y = 66 + 20 * row;
                text(names[id], X(22), Y(y), m.selection == row + 1 ? C(0xffffff) : title_gold, 42);
                const auto percent = [](int level) { return std::to_string(level * 10) + "%"; };
                const std::string value = id == 0 ? (m.sound ? percent(sound_volume) : on_off(false)) : id == 1 ? com(std::size_t(60 + m.game_speed), speed[std::clamp(m.game_speed, 0, 2)])
                    : id == 2 ? trim_word(m.instant_speech ? bank_text(3942, "Fast") : bank_text(3943, "Normal")) : id == 3 ? on_off(m.smooth_camera)
                    : id == 4 ? (m.language ? tr("Russian", "Русский") : tr("English", "Английский")) : id == 5 ? percent(music_volume) : id == 6 ? on_off(m.smooth_motion)
                    : id == 7 ? (m.modern_ui && m.wide_ui ? tr("Modern", "Новый") : m.wide_ui ? tr("Wide", "Широкий") : std::string("Java")) : id == 8 ? on_off(m.autosave)
                    : id == 10 ? percent(effects_volume) : on_off(m.rumble);
                text(value, X(97) - text_width(value) / 2, Y(y), title_gold, 60);
                if (left) blit(*left, X(65), Y(y));
                if (right) blit(*right, X(123), Y(y));
                hits.push_back({{X(57), Y(y - 4), 20, 18}, 660 + row * 2});
                hits.push_back({{X(115), Y(y - 4), 20, 18}, 661 + row * 2});
                hits.push_back({{X(18), Y(y - 4), 38, 18}, 680 + row});
            }
        }
        if (!main && m.page != TitlePage::loading) { // touch has no B: a Back button in the corner
            const Rect b{kW - 48, kH - 22, 45, 19};
            frame(b, panel);
            const std::string back_word = bank_text(3909, "Back");
            text(back_word, b.x + (b.w - text_width(back_word)) / 2, b.y + 5, ink);
            hits.push_back({b, 600});
        }
        if (!m.message.empty()) af_box(m.message);
    }
    DC_FlushRange(back, sizeof(back));
    dmaCopy(back, vram, sizeof(back));
}

void load_title_art(const GameArchive& archive) {
    const auto all = [&](const char* stem) {
        std::vector<std::size_t> ids;
        const auto atlas = load_sprite_atlas(archive, stem);
        for (std::size_t i = 0; i < atlas.frames.size(); ++i) ids.push_back(i);
        return ids;
    };
    load_color_frames(archive, "img/title1", {0, 2, 3, 4}, 0x700, art);
    load_color_frames(archive, "img/title2", {0, 1, 2, 3, 4}, 0x900, art);
    load_color_frames(archive, "sgui/mm/etc", all("sgui/mm/etc"), 0x800, art);
    load_color_frames(archive, "img/glb", {2, 3, 5, 11}, 0xB00, art);
    for (int i = 0; i < 3; ++i) {
        const auto image = decode_indexed_png(load_sprite_frame_png(archive, "sgui/mm/face", std::size_t(i)));
        ColorFrame colour{int(image.width), int(image.height), {}}, grey = colour;
        colour.px.resize(std::size_t(colour.w) * colour.h);
        grey.px.resize(colour.px.size());
        for (std::size_t p = 0; p < colour.px.size(); ++p) {
            const auto* c = &image.rgba[p * 4];
            if (c[3] < 128) continue;
            colour.px[p] = u16(to_rgb15(c) | 0x8000);
            const auto g = std::uint8_t((int(c[0]) * 30 + int(c[1]) * 59 + int(c[2]) * 11) / 100);
            const std::uint8_t gg[3]{g, g, g};
            grey.px[p] = u16(to_rgb15(gg) | 0x8000);
        }
        art[0xA00 + i] = std::move(colour);
        art[0xA10 + i] = std::move(grey);
        load_color_frames(archive, "grd/" + std::to_string(i), {0, 1}, 0xA20 + i * 2, art);
    }
}
} // namespace ui


} // namespace


// Language (Options, port row 4): the JAR's own text, or the built-in Russian pack (fonts, language
// bank, localized images), as main.cpp load_text_font / load_text_bank.
BitmapFont load_text_font(GameArchive& archive, const std::string& path) {
    const auto* pack = archive.has_language_pack() ? &russian_language_pack() : nullptr;
    archive.set_language_pack(nullptr);
    BitmapFont font = load_bitmap_font(archive, path);
    archive.set_language_pack(pack);
    recolor_bitmap_font(font, 0xffffff, 0x000000);
    if (pack) { // the JAR's Latin glyphs plus the Russian ones under their Cyrillic codepoints
        BitmapFont russian = load_bitmap_font(archive, path);
        recolor_bitmap_font(russian, 0xffffff, 0x000000);
        add_bitmap_glyphs(font, russian, russian_font_glyphs());
        add_marked_glyph(font, 0x418, 0x419, false); // the Russian fonts have no Й, Ё or ё
        add_marked_glyph(font, 0x415, 0x401, true);
        add_marked_glyph(font, 0x435, 0x451, true);
    }
    return font;
}
std::vector<std::string> load_text_bank(const GameArchive& archive) {
    auto bank = load_language_bank(archive, "lang/language.fr-FR");
    if (archive.has_language_pack()) for (auto& t : bank) t = decode_russian_font_text(t);
    if (bank.size() > 727 && bank[727] == "6") { // grdsk 3 (Rage) left as a bare "6" by the translation
        const std::string rage = "Сила атаки ненадолго возрастает.";
        bank[727] = archive.has_language_pack() ? rage : encode_russian_font_text(rage);
    }
    if (bank.size() > 1193 && (bank[1193] == "Надеть" || bank[1193] == encode_russian_font_text("Надеть")))
        bank[1193] = archive.has_language_pack() ? std::string("Надет") : encode_russian_font_text("Надет"); // gm 32: the active guardian
    struct TextFix {std::size_t id; const char* original; const char* fixed;};
    static const TextFix fixes[]={
        // itm/mixtbl: will stone + water crystal + ruby -> (17,7) Orb of faint (392), not weakening (388).
        {1881,"[Рецепт] Сфера Слабости = Камень Воли + Кристалл Воды + Рубин",
              "[Рецепт] Сфера обморока = камень воли + кристалл воды + рубин"},
        {1856,"[Рецепт] Камень Жизни = корень + платина","[Рецепт] Камень здоровья = корень + платина"},
        {1305,"Николас сказал мне поймать 5 волков и принести 5 зубов. Если я его исполню его просьбу – он обучит меня новым навыкам",
              "Николас велел поймать пять волков и принести пять драконьих зубов. Если пройду испытание, он научит меня новому приёму."},
        {1410,"Крестоносцы томятся в подземной тюрьме Лакуса. С помощью Ланака освободите их",
              "Крестоносцы заперты в подземной тюрьме Солума. Освободим их вместе с Ланаком."},
        {1417,"Нужно узнать больше о Хранителях.Может мама Лесли что-нибудь знает",
              "Нужно больше узнать о Хранителях. Может, мама или Лесли что-нибудь знают."},
        {3139,"|[Рея] Я должна идти первой...","|[Рея] К кому пойти сначала?.."},
        {1473,"Найджел сказал,что видел пещеру в Эребос в Центральных горах",
              "Найджел сказал, что из пещеры в Центральных горах можно увидеть Эребос."},
        {3696,"$[Нед] Давай быстрее! Шевелись!","$[Нед] Уже вернулся? Отлично!"},
        // Lever (21,3): m/6/54.evt and m/8/54.evt use it; the messages report what was just done.
        {465,"|Предмет квеста;неработающий рычаг,который доставляет в нужное место.",
             "|Предмет задания;Пригодный к использованию рычаг, который можно вставить в подходящее гнездо."},
        {3486,"$[Арамор] Во всех колоннах не хватает кирпичей. Я могу вставить в них что-нибудь...",
              "$[Арамор] В каждой колонне не хватает одного кирпича. Можно что-нибудь вставить в эти места..."},
        {3497,"Выключите переключатель на левой колонне используя рычаг.","Переключатель на левой колонне выключен с помощью рычага."},
        {3498,"Включите переключатель на левой колонне используя рычаг.","Переключатель на левой колонне включён с помощью рычага."},
        {3499,"Выключите переключатель на средней колонне используя рычаг.","Переключатель на средней колонне выключен с помощью рычага."},
        {3500,"Включите переключатель на средней колонне используя рычаг.","Переключатель на средней колонне включён с помощью рычага."},
        {3501,"Выключите переключатель на правой колонне используя рычаг.","Переключатель на правой колонне выключен с помощью рычага."},
        {3502,"Включите переключатель на правой колонне используя рычаг.","Переключатель на правой колонне включён с помощью рычага."},
        {3503,"Вставьте рычаг в колонну.","Рычаг вставлен в колонну."},
        {3504,"Я не могу. Попробуйте использовать рычаг снова.","Не могу спуститься. Нужно снова воспользоваться рычагом."},
    };
    if (archive.has_language_pack())
        for (const auto& fix : fixes) if (fix.id < bank.size() && bank[fix.id] == fix.original) bank[fix.id] = fix.fixed;
    return bank;
}
int loaded_language = -1;
void load_texts(GameArchive& archive) {
    archive.set_language_pack(title.language == 1 ? &russian_language_pack() : nullptr);
    loaded_language = text_language = title.language;
    text_bank = load_text_bank(archive);
    com_text = parse_tdf_text(archive.read_entry("sgui/com.tdf"), text_bank);
    ui::location_names = parse_tdf_text(archive.read_entry("m/name.tdf"), text_bank);
    ui_font = load_text_font(archive, "fonts/small.mf");
    hero_text = parse_tdf_text(archive.read_entry("char/hero.tdf"), text_bank);
    guardian_text = parse_tdf_text(archive.read_entry("grd/grd.tdf"), text_bank);
    guardian_skill_names = parse_tdf_text(archive.read_entry("grd/grdsk.tdf"), text_bank);
    menu_text = parse_tdf_text(archive.read_entry("sgui/gm.tdf"), text_bank);
    shop_ui_text = parse_tdf_text(archive.read_entry("sgui/shop.tdf"), text_bank);
    blacksmith_text = parse_tdf_text(archive.read_entry("sgui/blak.tdf"), text_bank);
    refiner_text = parse_tdf_text(archive.read_entry("sgui/refi.tdf"), text_bank);
    attribute_text = parse_tdf_text(archive.read_entry("itm/itmatt.tdf"), text_bank);
    help_text = parse_tdf_text(archive.read_entry("sgui/help.tdf"), text_bank);
    // bh.d/e: fonts/big.mf as it is. The Russian release writes its headings (3920.., help topics) in
    // the big font's own layout (MOBAR ULPA = НОВАЯ ИГРА), so that font is not merged with Latin.
    heading_font = load_bitmap_font(archive, "fonts/big.mf");
    if (heading_font.has_shadow_palette) { // the fill is the rarer of the two colours (the Russian font swaps them)
        auto& rgba = heading_font.atlas.rgba;
        const auto rgb_at = [&](std::size_t q) { return (std::uint32_t(rgba[q]) << 16) | (std::uint32_t(rgba[q + 1]) << 8) | rgba[q + 2]; };
        std::size_t fill = 0, outline = 0;
        for (std::size_t q = 0; q + 3 < rgba.size(); q += 4) {
            if (!rgba[q + 3]) continue;
            if (rgb_at(q) == heading_font.text_palette_rgb) ++fill; else if (rgb_at(q) == heading_font.shadow_palette_rgb) ++outline;
        }
        if (fill > outline) std::swap(heading_font.text_palette_rgb, heading_font.shadow_palette_rgb);
    }
    recolor_bitmap_font(heading_font, 0xffffff, 0x000000);
    quest_text_class = -1; // sgui/q<class> reloads with the menu
}

namespace { // the declarations above are in this unnamed namespace
// ---------------------------------------------------------------------------------------------
// Ending (ah GAMEOVER -> as.g): /sgui/ed<hero> story pages after a white fade, then the /sgui/edsr
// staff roll beside img/end (ending.hpp), on the top screen; the bottom screen stays black. Then
// n.c(): bank this run's progress, unlock Reah/Kail, Hardcore restarts the story at the hero's start
// with level, gear and guardians kept and quest items removed (main.cpp start/finish_ending).
std::optional<EndingSequence> ending;
int current_music = -2; // bw slot playing as the map / title music
// bw: title 22, ending 23, otherwise ae.h by tileset (maps 79..81: 4); silent at game over (main.cpp).
void update_music() {
    static constexpr int kMapMusic[20] = {28, 0, 27, 29, -1, 1, 26, 31, 31, 2, -1, 25, 24, 24, 30, 3, -1, -1, -1, -1};
    int wanted = -1;
    if (ending) wanted = 23;
    else if (title.active && !title.in_game) wanted = 22;
    else if (!game_over) wanted = world.map_id >= 79 && world.map_id <= 81 ? 4 : world.map.kind < 20 ? kMapMusic[world.map.kind] : -1;
    if (wanted != current_music) { current_music = wanted; snd::play_music(snd::map_music, wanted); }
    if (!ev.runtime && snd::music_playing(snd::event_music)) snd::stop_music(snd::event_music); // ah.c()
    snd::set_enabled(title.sound);
    snd::set_volume(sound_volume, music_volume, effects_volume);
}
bool ending_running() { return ending.has_value(); }
void start_ending(const GameArchive& archive) {
    const int hero = game.character_class;
    std::vector<std::string> story, staff;
    try {
        story = parse_tdf_text(archive.read_entry("sgui/ed" + std::to_string(hero) + ".tdf"), text_bank);
        staff = parse_tdf_text(archive.read_entry("sgui/edsr.tdf"), text_bank);
        combat_art.erase(0xa00);
        load_color_frames(archive, "img/end", {std::size_t(hero - 6)}, 0xa00 - (hero - 6), combat_art);
    } catch (const std::exception& error) { log_file(std::string("ending load failed: ") + error.what()); }
    ending.emplace();
    ending->start(std::move(story), std::move(staff));
    snd::stop_all(); current_music = -2; // as.g(): bw.f(), then 23
    game_menu = false; shop.active = false; service.reset();
    if (world.hero) world.hero->release();
    show_title_screens(true);
    ui_dirty = true;
    log_file("ending started");
}

void top_text(const std::string& s, int x, int y, u16 colour) {
    const auto& atlas = ui_font.atlas;
    for (const auto& g : layout_bitmap_text(ui_font, s, 0, 0, 4096))
        for (int yy = 0; yy < std::min<int>(ui_font.height, int(atlas.height)); ++yy)
            for (int xx = 0; xx < int(g.source_width); ++xx) {
                const auto* p = &atlas.rgba[(std::size_t(yy) * atlas.width + g.source_x + xx) * 4];
                if (p[3] < 128 || (p[0] < 64 && p[1] < 64 && p[2] < 64)) continue;
                const int px = x + g.x + xx, py = y + g.y + yy;
                if (px >= 0 && py >= 0 && px < 256 && py < 192) top_buffer[std::size_t(py) * 256 + px] = colour;
            }
}
int top_text_width(const std::string& s) {
    int w = 0;
    for (const auto& g : layout_bitmap_text(ui_font, s, 0, 0, 4096)) w = std::max(w, g.x + int(g.source_width));
    return w;
}

void draw_ending_top() { // as.d / as.e
    const auto& e = *ending;
    const int lw = 256, lh = 192;
    const auto line = [&](const std::string& t, int y) { top_text(t, lw / 2 - top_text_width(t) / 2, y, u16(0xffff)); };
    // anchor: 17 top-centre, 40 bottom-right, 6 left-vcentre
    const auto picture = [&](const ColorFrame* f, int x, int y, int anchor, bool mirror, int clip_top = 0, int clip_bottom = 192) {
        if (!f) return;
        if (anchor == 17) x -= f->w / 2; else if (anchor == 40) { x -= f->w; y -= f->h; } else if (anchor == 6) y -= f->h / 2;
        for (int yy = 0; yy < f->h; ++yy)
            for (int xx = 0; xx < f->w; ++xx) {
                const u16 v = f->px[std::size_t(yy) * f->w + (mirror ? f->w - 1 - xx : xx)];
                const int px = x + xx, py = y + yy;
                if (v && px >= 0 && px < 256 && py >= clip_top && py < clip_bottom) top_buffer[std::size_t(py) * 256 + px] = v;
            }
    };
    if (const int fade = e.fade_level(); fade >= 0) {
        const int c = fade >> 3;
        top_buffer.assign(256 * 192, u16(RGB15(c, c, c) | 0x8000));
    } else {
        top_buffer.assign(256 * 192, u16(0x8000));
        if (e.phase == EndingSequence::Phase::credits) {
            picture(art_frame(combat_art, 0xa00), 0, lh / 2, 6, false);
            for (const auto& roll : e.rolling) line(roll.text, roll.y);
        } else {
            std::vector<std::string> page;
            for (int i = e.first; i < e.end && i < int(e.story.size()); ++i) page.push_back(e.story[std::size_t(i)]);
            int y = lh / 2 - (int(page.size()) + 1) * 15 / 2;
            const int hero = game.character_class;
            if (hero == 6 && (e.first == 2 || e.first == 6 || e.first == 9 || e.first == 13)) {
                // Ronin: grey band with his face (m/face 0, mirrored by br.c); page 9 slides in face 8.
                for (int yy = 15; yy < 55; ++yy) for (int xx = 0; xx < 256; ++xx) top_buffer[std::size_t(yy) * 256 + xx] = u16(RGB15(23, 23, 23) | 0x8000);
                picture(art_frame(ui_art, 0x100), lw / 4, 5, 17, true, 15, 55);
                if (e.first == 9) picture(art_frame(ui_art, 0x108), lw / 4 * 3 + (e.counter >= 27 ? (e.counter - 27) * 10 : 0), 5, 17, true, 15, 55);
                y += 30;
            } else if (hero == 8) picture(art_frame(ui_art, 0x117), lw, lh, 40, false);
            for (const auto& t : page) { line(t, y); y += 15; }
        }
    }
    top_dirty = true;
}

void finish_ending(const GameArchive& archive) {
    ending.reset();
    const auto profile = make_new_game_profile(game.character_class);
    SaveGame next = capture_save_raw();
    next.clear_progress = std::min(100, game.clear_progress + run_progress_percent(next));
    next.new_game_plus = 1;
    next.map_id = profile.map_id; next.zone = profile.character_class; next.x = profile.x; next.y = profile.y;
    next.direction = profile.direction; next.switches = profile.switches; next.quests = profile.quests;
    next.inventory.erase(std::remove_if(next.inventory.begin(), next.inventory.end(),
                                        [](const InventoryStack& st) { return st.item_type >= 18 && st.item_type <= 21; }), next.inventory.end()); // g.c(): ad.b()
    next.vitals.hp = next.vitals.max_hp; next.vitals.sp = next.vitals.max_sp;
    title.heroes_unlocked = true;
    save_settings();
    show_title_screens(false);
    try { restore_save(archive, next); }
    catch (const std::exception& error) { log_file(std::string("Hardcore start failed: ") + error.what()); }
    ui_dirty = true;
    log_file("ending finished: Hardcore mode");
}
} // namespace

int main(int argc, char** argv) {
    irqSet(IRQ_VBLANK, on_vblank);
    irqEnable(IRQ_VBLANK);
    videoSetMode(MODE_5_2D);
    videoSetModeSub(MODE_5_2D);
    vramSetBankE(VRAM_E_MAIN_BG);              // 64 KB at 0x06000000: BG0 bands
    vramSetBankA(VRAM_A_MAIN_BG_0x06020000);   // 128 KB: the field bitmap
    vramSetBankB(VRAM_B_MAIN_SPRITE);          // 128 KB: 16 bitmap sprites
    vramSetBankC(VRAM_C_SUB_BG);               // 128 KB: the bottom screen, 16 bpp
    vramSetBankD(VRAM_D_MAIN_BG_0x06060000);   // 128 KB: the boss layer, 16 bpp
    const int bg_sub = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    ui::vram = bgGetGfxPtr(bg_sub);
    bg_main = bgInit(3, BgType_Bmp8, BgSize_B8_512x256, 8, 0); // base 8 x 16 KB = VRAM A
    bgWrapOn(bg_main);
    set_bg2_bitmap16(false); // base 24 x 16 KB = VRAM D
    cpuStartTiming(0); // free-running timers 0+1 for the frame profile
    init_overlay();
    show_field(false);
    oamInit(&oamMain, SpriteMapping_Bmp_1D_128, false);
    for (auto& slot : slots) slot.gfx = oamAllocateGfx(&oamMain, SpriteSize_64x64, SpriteColorFormat_Bmp);
    for (auto& slot : small_slots) slot.gfx = oamAllocateGfx(&oamMain, SpriteSize_32x32, SpriteColorFormat_Bmp);
    ui::message("");

    const auto fail = [](const std::string& text) { // no game font yet: the text console
        consoleDemoInit();
        std::printf("Heroes Lore DS\n\n%s\n", text.c_str());
        while (pmMainLoop()) swiWaitForVBlank();
    };
    const bool fat = fatInitDefault();
    // Saves, settings and the log: heroeslore/ beside the .nds when the loader passes its path.
    std::string nds_dir, data_dir;
    if (argc > 0 && argv && argv[0] && std::strchr(argv[0], '/')) {
        nds_dir.assign(argv[0], std::strrchr(argv[0], '/') + 1);
        data_dir = nds_dir + "heroeslore/";
    }
    const std::string jar = find_jar(data_dir, nds_dir);
    if (jar.empty()) {
        fail(std::string(fat ? "" : "FAT init failed (no SD/DLDI).\n") +
             "Heroes_Lore.jar not found.\nPut it in heroeslore/ beside\nthe .nds, in /heroeslore/ or\nthe SD card root.");
        return 0;
    }
    if (data_dir.empty()) data_dir = jar.rfind("embedded:", 0) == 0 ? "/heroeslore/" : jar.substr(0, jar.find_last_of('/') + 1);
    mkdir(data_dir.substr(0, data_dir.size() - 1).c_str(), 0777); // exists already: harmless
    log_path = data_dir + "heroes_lore_nds.log";
    if (FILE* file = std::fopen(log_path.c_str(), "w")) std::fclose(file); else log_path.clear();
    sample_memory();
    {
        char text[160];
        std::snprintf(text, sizeof(text), "Heroes Lore DS, %s mode, JAR %s, heap total %u KB free %u KB, sprite slots %d",
                      isDSiMode() ? "DSi" : "DS", jar.c_str(), unsigned(memory.total / 1024), unsigned(memory.free / 1024),
                      int(std::count_if(slots.begin(), slots.end(), [](const SpriteSlot& s) { return s.gfx != nullptr; })));
        log_file(text);
    }
    std::string error_text;
    try {
        GameArchive archive(jar);
        current_archive = &archive;
        save_dir = data_dir;
        load_settings();
        snd::init([&archive](const std::string& name) { return archive.read_entry(name); }, log_file);
        load_texts(archive);
        ui::load_art(archive);
        try { enemy_stats = combat::parse_enemy_table(archive.read_entry("enm/data0")); } // ae: /enm/data<g>
        catch (const std::exception& error) { log_file(std::string("enemy table: ") + error.what()); }
        try { item_catalog = load_item_catalog(archive); }
        catch (const std::exception& error) { log_file(std::string("item catalog: ") + error.what()); }
        try { mix_table = parse_mix_table(archive.read_entry("itm/mixtbl")); } catch (const std::exception&) {}
        ui::load_title_art(archive);
        top_buffer.assign(256 * 192, 0xffff);

        // Title: TitleMenu drives the pages; the logo animates once per logic tick.
        const auto go_to_title = [&]() {
            end_event();
            game_over = false;
            scan_saves();
            title.active = true;
            title.open_menu();
            show_title_screens(true);
            draw_title_top();
            ui::draw_title();
        };
        const auto title_action = [&](TitleAction action) {
            if (action == TitleAction::continue_game) {
                const auto* entry = title.selected_save();
                if (!entry) return;
                try {
                    const auto saved = read_save(entry->autosave ? autosave_path() : save_path_for(entry->hero));
                    title.active = false;
                    show_title_screens(false);
                    restore_save(archive, saved);
                    ui_dirty = true;
                } catch (const std::exception& error) {
                    log_file(std::string("load failed: ") + error.what());
                    go_to_title();
                    title.message = error.what();
                }
            } else if (action == TitleAction::start_game) {
                title.active = false;
                show_title_screens(false);
                begin_new_game(archive, title.character_class, title.chosen_guardians);
                ui_dirty = true;
            } else if (action == TitleAction::quit) {
                title.page = TitlePage::logo; // the DS has nowhere to quit to: back to the logo
            }
        };
        scan_saves();
        show_title_screens(true);
        draw_title_top();
        ui::draw_title();
        int title_acc = 0;

        int held_direction = 0, frames = 0, logic_acc = 0;
        int glide_x = 0, glide_y = 0; bool glide_valid = false; // detached camera before the last tick
        u32 second_start = vblanks;
        while (pmMainLoop()) {
            update_music();
            scanKeys();
            const u32 down = keysDown(), held = keysHeld();
            int action = 0;
            if (down & KEY_TOUCH) {
                touchPosition touch;
                touchRead(&touch);
                action = ui::hit_at(touch.px, touch.py);
            }
            if (title.active) {
                bool changed = false;
                if (title.page == TitlePage::logo) {
                    if ((down & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_START | KEY_SELECT | KEY_L | KEY_R | KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) || action) {
                        title.confirm();
                        changed = true;
                    }
                } else {
                    const int direction = (down & KEY_UP) ? 1 : (down & KEY_DOWN) ? 2 : (down & KEY_LEFT) ? 3 : (down & KEY_RIGHT) ? 4 : 0;
                    if (direction) { options_navigate(direction); changed = true; }
                    if (title.page == TitlePage::options && (down & (KEY_L | KEY_R))) { // L/R page the Options tabs
                        title.switch_options_tab((down & KEY_L) ? -1 : 1); changed = true;
                    }
                    if (title.page == TitlePage::options && (down & KEY_A)) { options_navigate(4); changed = true; }
                    else if (down & KEY_A) { title_action(title.confirm()); changed = true; }
                    if (down & KEY_B) { title.back(); changed = true; }
                    if (action) { // touch: pick the row / face / guardian, then confirm
                        changed = true;
                        if (action == 1) title_action(title.confirm());
                        else if (action == 3 || action == 4) { title.yes = action == 3; title_action(title.confirm()); }
                        else if (action >= 100 && action < 106) {
                            if (action - 100 == 1 && !title.continue_available) title.message = tr("No saved game.", "Нет сохранений.");
                            else { title.selection = action - 100; title_action(title.confirm()); }
                        }
                        else if (action >= 200 && action < 203) {
                            if (title.character_class == 6 + action - 200) title_action(title.confirm());
                            else title.character_class = 6 + action - 200;
                        }
                        else if (action >= 300 && action < 303) { title.guardian_focus = action - 300; title_action(title.confirm()); }
                        else if (action >= 400 && action < 410) {
                            if (title.load_selection == action - 400) title_action(title.confirm());
                            else title.load_selection = action - 400;
                        }
                        else if (action >= 500 && action < 504) { title.selection = action - 500; title_action(title.confirm()); }
                        else if (action == 600) title.back();
                        else if (action == 650 || action == 651) { title.selection = 0; title.navigate(action == 650 ? 3 : 4); }
                        else if (action >= 660 && action < 666) { title.selection = (action - 660) / 2 + 1; options_navigate((action - 660) % 2 ? 4 : 3); }
                        else if (action >= 680 && action < 683) title.selection = action - 680 + 1;
                    }
                }
                if (changed && title.page == TitlePage::options) {
                    save_settings();
                    if (title.language != loaded_language) {
                        try { load_texts(archive); ui::load_art(archive); }
                        catch (const std::exception& error) { log_file(std::string("language: ") + error.what()); }
                    }
                }
                if (title.active) {
                    title_acc += 1667;
                    if (title_acc >= 7100) {
                        title_acc -= 7100;
                        title.logo.tick(256, 192);
                        title.tick_brush();
                        if (!title.in_game) draw_title_top(); // in-game Help / Options keep the field on top
                        changed = true;
                    }
                    if (changed) ui::draw_title();
                    swiWaitForVBlank();
                    title_flush();
                    continue;
                }
                save_settings();
                held_direction = 0; frames = 0; second_start = vblanks; logic_acc = 0;
                ui_dirty = true; // back from the title pages (or into a new game): repaint the bottom screen
                continue;
            }
            if (game_over) { // the game-over screen on top, then the title (Load / New Game)
                if (game_over_frames == 17) {
                    show_field(false);
                    bgHide(bg_overlay);
                    hide_caption();
                    set_bg2_bitmap16(true);
                    bgSetScroll(bg_boss, 0, 0);
                    bgUpdate();
                    draw_game_over(0);
                    bgShow(bg_boss);
                    --game_over_frames;
                    logic_acc = 0;
                }
                logic_acc += 1667;
                if (logic_acc >= 7100) {
                    logic_acc -= 7100;
                    --game_over_frames;
                    draw_game_over((17 - game_over_frames) * 2);
                }
                swiWaitForVBlank();
                title_flush();
                if (game_over_frames <= 0) go_to_title();
                continue;
            }
            if (ending) { // as states 12/13: any key or touch turns the story page; one tick per logic tick
                if ((down & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_START | KEY_L | KEY_R)) || (down & KEY_TOUCH)) ending->press();
                logic_acc += 1667;
                const int tick = int(title.tick_ms()) * 100;
                bool redraw = false;
                while (logic_acc >= tick) { logic_acc -= tick; ending->tick(192); redraw = true; }
                if (redraw) draw_ending_top();
                if (ui_dirty) { ui::draw(); ui_dirty = false; }
                swiWaitForVBlank();
                title_flush();
                if (ending->phase == EndingSequence::Phase::done) finish_ending(archive);
                continue;
            }
            bool loaded = false;
            const auto dialogue_page_or_advance = [&]() {
                const bool portrait = ev.portrait != 0 && ui::art.count(0x100 + std::abs(ev.portrait) - 1);
                if (!ev.item_message && ui::line_count(ev.text, ui::dialogue_text_w(portrait)) > (ev.page + 1) * ui::dialogue_rows()) {
                    ++ev.page;
                    ui_dirty = true;
                } else ev.advance = true;
            };
            if (!ev.status.empty()) {
                if ((down & (KEY_B | KEY_A)) || action == ui::kOk) {
                    ev.status.clear();
                    end_event();
                    if (game_over) { go_to_title(); continue; } // ao.q -> n.a(16): back to the title (Load / New Game)
                }
            } else if (screen_open()) {
                if (down || action) { screen_input(down, action); ui_dirty = true; }
                tick_service();
            } else if (game_menu) {
                if (down || action) {
                    if (game_menu_input(archive, down, action)) request_title = true;
                    ui_dirty = true;
                }
                if (request_title) { request_title = false; go_to_title(); continue; }
            } else if (ev.choice) {
                if (down & (KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN)) { ev.choice_yes = !ev.choice_yes; ui_dirty = true; }
                if (down & KEY_A) ev.choice_response = ev.choice_yes;
                if ((down & KEY_B) || action == ui::kNo) ev.choice_response = false;
                if (action == ui::kYes) ev.choice_response = true;
            } else if (ev.dialogue) {
                if ((down & KEY_A) || action == ui::kNext || action == ui::kOk) dialogue_page_or_advance();
                if (((down & KEY_B) || action == ui::kSkip) && ev.runtime && !ev.item_message) { ev.runtime->request_skip(); ev.advance = true; }
                else if (((down & KEY_B)) && ev.item_message) ev.advance = true;
            } else if (ev.runtime) {
                if (down & KEY_B) ev.runtime->request_skip();
            } else {
                if (down & KEY_A) try_button_event();
                if (down & KEY_X) try_skill_attack();
                // As the other ports: guardians on the free face buttons (B calm, Y wild), L the quick item, R the next one.
                if ((down & (KEY_B | KEY_Y)) || action == ui::kCalm || action == ui::kWild)
                    if (combat_world.summon_guardian(combat_view(), (down & KEY_B) || action == ui::kCalm)) {
                        char line[96];
                        std::snprintf(line, sizeof(line), "guardian %d skill %d summoned on map %d", combat_world.guardian().id,
                                      combat_world.guardian().skill, world.map_id);
                        log_file(line);
                    }
                if ((down & KEY_R) || action == ui::kNextItem) { // g.a: cycle the quick item type 7..10
                    ui::quick_item_type = ui::quick_item_type >= 10 ? 7 : ui::quick_item_type + 1;
                    ui_dirty = true;
                }
                if ((down & KEY_L) || action == ui::kUseItem) {
                    if (const auto slot = quick_item_slot(game.inventory, ui::quick_item_type)) {
                        use_consumable(game.inventory, *slot, game.vitals);
                        game.inventory.erase(std::remove_if(game.inventory.begin(), game.inventory.end(),
                                                            [](const InventoryStack& s) { return s.count <= 0; }),
                                             game.inventory.end());
                        ui_dirty = true;
                    }
                }
                // open_menu: a hero mid-step finishes it first (menu_requested), as main.cpp.
                if ((down & KEY_START) || action == ui::kMenu) {
                    menu_requested = true;
                    world.hero->release(); held_direction = 0;
                }
                if (menu_requested) {
                    const auto h = world.hero->state();
                    if (h.motion_mode == 1 && !h.partial_x && !h.partial_y) {
                        menu_requested = false;
                        open_game_menu(archive);
                        ui_dirty = true;
                        continue;
                    }
                }
            }
            if (loaded) { held_direction = 0; frames = 0; second_start = vblanks; logic_acc = 0; }

            const int direction = (held & KEY_UP) ? 1 : (held & KEY_DOWN) ? 2 : (held & KEY_LEFT) ? 3 : (held & KEY_RIGHT) ? 4 : 0;
            const bool free_walk = !ev.runtime && ev.status.empty() && !menu_requested && !game_menu && !screen_open(); // the D-pad moves the menu cursor
            if (direction != held_direction) {
                if (free_walk) { if (direction) world.hero->press(direction); else world.hero->release(); }
                held_direction = direction;
            }
            // ah Skip: one logic tick per frame until the script ends.
            const int tick = int(title.tick_ms()) * 100; // Options Game speed: bs.a 125 / 100 / 71 ms
            logic_acc += game_menu || screen_open() ? 0 : ev.runtime && ev.runtime->skipping() ? tick : 1667; // 1/60 s in 10 us units; tick 71 ms
            const int map_before = world.map_id;
            const u32 prof_t0 = cpuGetTiming();
            while (logic_acc >= tick) {
                logic_acc -= tick;
                // ah.b() moves a detached camera once per tick: glide between its last two positions (main.cpp glide).
                glide_valid = ev.runtime && !ev.runtime->camera.follow;
                if (glide_valid) { glide_x = ev.runtime->camera.x; glide_y = ev.runtime->camera.y; }
                logic_tick(archive);
                if (world.map_id != map_before) { logic_acc = 0; frames = 0; second_start = vblanks; held_direction = 0; break; }
            }

            int cx, cy; // camera: the hero, or the script's detached camera
            if (ev.runtime) {
                cx = ev.runtime->camera.x + 8; cy = ev.runtime->camera.y + 8;
                if (glide_valid && !ev.runtime->camera.follow && std::abs(cx - 8 - glide_x) <= 48 && std::abs(cy - 8 - glide_y) <= 48) {
                    cx = glide_x + 8 + (cx - 8 - glide_x) * logic_acc / tick;
                    cy = glide_y + 8 + (cy - 8 - glide_y) * logic_acc / tick;
                }
            }
            else { const auto h = world.hero->state(); cx = h.x_pixels + 8; cy = h.y_pixels + 8; }
            const int map_w = world.map.width * 16, map_h = world.map.height * 16;
            const int sx = map_w <= kScreenW ? -(kScreenW - map_w) / 2 : std::clamp(cx - kScreenW / 2, 0, map_w - kScreenW);
            int sy = map_h <= kScreenH ? -(kScreenH - map_h) / 2 : std::clamp(cy - kScreenH / 2, 0, map_h - kScreenH);
            if (dual_arena()) { // top: rows [sy, sy+192), bottom: the next 120 rows
                sy = std::clamp(cy - 192 - ui::kArenaH / 2, 0, std::max(0, map_h - 192 - ui::kArenaH));
                arena_sx = sx; arena_oy = sy;
            }
            view_x = sx; view_y = sy;
            const u32 prof_t1 = cpuGetTiming();
            prof_logic += prof_t1 - prof_t0;
            update_ring(sx, sy);
            draw_entities(sx, sy, world.visuals.world_hidden);
            const u32 prof_t2 = cpuGetTiming();
            prof_sprites += prof_t2 - prof_t1;
            draw_boss_layer(sx, sy, world.visuals.world_hidden || pending_show || !field_shown);
            prof_boss += cpuGetTiming() - prof_t2;

            ++frames;
            const u32 now = vblanks;
            if (now - second_start >= 60) {
                const int frames_counted = frames;
                ui::fps = int(frames * 60 / (now - second_start)); frames = 0; second_start = now;
                fps_min = fps_samples ? std::min(fps_min, ui::fps) : ui::fps; fps_sum += ui::fps; ++fps_samples;
                if (!boss_animations.empty()) { // boss maps: display state, once a second (DS boss layer diagnostics)
                    int bosses = 0;
                    for (const auto& e : combat_world.enemies()) bosses += e.boss != combat::BossKind::none && e.state != combat::EnemyState::dead;
                    const auto ms = [&](u32 t) { return int(t / std::max(1, frames_counted) / 3351); }; // 0.1 ms per frame
                    char line[320];
                    std::snprintf(line, sizeof(line),
                        "boss diag: fps %d bosses %d box %d,%d-%d,%d | 0.1 ms per frame: logic %d sprites %d boss %d ui %d work %d max %d"
                        " | boss redraws %d arena draws %d (in 1 s) | per arena draw 0.1 ms: bg %d actors %d copy %d"
                        " | sound thread %d us/s voices %d peak %d",
                        ui::fps, bosses, boss_box[0], boss_box[1], boss_box[2], boss_box[3], ms(prof_logic), ms(prof_sprites), ms(prof_boss),
                        ms(prof_ui), ms(prof_work), int(prof_max / 3351), prof_boss_draws, prof_arena_draws,
                        int(prof_arena_bg / std::max(1, prof_arena_draws) / 3351), int(prof_arena_actors / std::max(1, prof_arena_draws) / 3351),
                        int(prof_arena_copy / std::max(1, prof_arena_draws) / 3351), int(snd::stats().busy_us),
                        int(snd::stats().voices), int(snd::stats().voices_peak));
                    log_file(line);
                }
                prof_logic = prof_sprites = prof_boss = prof_ui = prof_work = prof_max = 0;
                prof_boss_draws = prof_arena_draws = 0;
                prof_arena_bg = prof_arena_actors = prof_arena_copy = 0;
                sample_memory();
                if (!ui_dirty) ui::update_status();
            }
            {   // the enemy bar follows the targeted enemy's HP; notices expire
                static int shown_target_hp = -1;
                const auto* target = combat_world.target_enemy();
                const int hp = target ? target->hp + (target->stats << 16) : -1;
                if (hp != shown_target_hp) { shown_target_hp = hp; ui_row_dirty = true; }
                static int shown_channel = -1;
                const auto& g = combat_world.guardian();
                const int channel = g.state == 2 && g.loops_total > 0 ? g.id * 4096 + g.skill * 256 + g.loops : -1;
                if (channel != shown_channel) { shown_channel = channel; ui_row_dirty = true; }
                static int shown_cooldowns = -2;
                if (const int k = ui::guardian_buttons_key(); k != shown_cooldowns) { shown_cooldowns = k; guardian_buttons_dirty = true; }
                static bool notice_shown = false;
                const bool notice_now = !notice.empty() && std::int32_t(notice_until - vblanks) > 0;
                if (notice_now != notice_shown) { notice_shown = notice_now; ui_row_dirty = true; }
            }
            try_autosave();
            {   // the STAT plate on MENU appears / goes with the attribute points
                static bool shown_points = false;
                if ((game.progress.attribute_points > 0) != shown_points) { shown_points = !shown_points; if (!ui_dirty) ui::update_menu_button(); }
            }
            const u32 prof_t3 = cpuGetTiming();
            if (ui_dirty) { ui::draw(); ui_dirty = ui_row_dirty = arena_dirty = guardian_buttons_dirty = false; }
            else if (arena_dirty && !boss_flip_pending) { ui::draw_arena_part(); arena_dirty = false; ++prof_arena_draws; }
            else if (ui_row_dirty) { ui::update_row(); ui_row_dirty = false; }
            else if (guardian_buttons_dirty) { ui::update_guardian_buttons(); guardian_buttons_dirty = false; }
            else if (world.hero) ui::move_hero_marker(world.hero->state().x_tile, world.hero->state().y_tile);
            {
                const u32 t = cpuGetTiming();
                prof_ui += t - prof_t3;
                prof_work += t - prof_frame_start;
                prof_max = std::max(prof_max, t - prof_frame_start);
            }
            swiWaitForVBlank();
            prof_frame_start = cpuGetTiming();
            boss_flush();
            setBrightness(1, world.visuals.flash_white ? 16 : 0);
            if (field_shown && !pending_show) {
                const bool hide = world.visuals.world_hidden;
                if (hide != field_hidden_by_event) {
                    field_hidden_by_event = hide;
                    if (hide) { bgHide(bg_main); REG_DISPCNT &= ~DISPLAY_SPR_ACTIVE; bgHide(bg_boss); }
                    else { bgShow(bg_main); REG_DISPCNT |= DISPLAY_SPR_ACTIVE; if (boss_layer_on) bgShow(bg_boss); }
                }
                if (hide && (ev.dialogue || ev.choice) && !ev.text.empty()) show_caption(ev.text);
                else hide_caption();
            }
            bgSetScroll(bg_main, sx + world.visuals.shake_x, sy + world.visuals.shake_y);
            bgSetScroll(bg_boss, boss_scroll_x + world.visuals.shake_x, boss_scroll_y + world.visuals.shake_y);
            bgUpdate();
            oamUpdate(&oamMain);
            if (pending_show && map_art.origin_x != kNoOrigin) { // scrolled onto the drawn window this vblank
                bgHide(bg_overlay);
                show_field(true);
                pending_show = false;
                field_hidden_by_event = false;
            }
        }
    } catch (const std::exception& error) {
        error_text = error.what();
    }
    log_file("error: " + error_text);
    fail("error: " + error_text);
    return 0;
}
