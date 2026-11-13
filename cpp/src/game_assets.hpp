#pragma once
// Textures of the field: terrain, objects, NPCs, enemies, bosses and the hero.
#include <SDL.h>
#include <array>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "character_sprite.hpp"
#include "eif.hpp"
#include "equipment.hpp"
#include "map.hpp"
#include "npc_sprite.hpp"
#include "sprite.hpp"

using SpriteTextures = std::unordered_map<int, SDL_Texture*>;
struct NpcVisualAssets {
    SpriteTextures textures;
    std::unordered_map<int, NpcSpriteAnimations> animations;
    SpriteTextures enemy_textures;
    std::unordered_map<int, std::vector<NpcSpriteLayer>> enemy_idle_frames;
    std::unordered_map<int, NpcSpriteAnimations> enemy_animations;
    std::unordered_map<int, BossSpriteAnimations> boss_animations;
    std::unordered_map<int, EifAnimation> effects; // enm/atef/NN and boss/atef/NN by sprite id
};

struct GuardianFxAssets {
    std::array<EifAnimation, 3> slots;  // ce.d: grd/spr/{id}_{n}.eif (id 0 uses 0_01 for slot 0)
    EifAnimation extra;                 // ce.m: grd/spr/0_02.eif, effect kind 2
    int atlas{};                        // 0 grd/fi, 1 grd/wa, 2 grd/gr (by id % 3)
};
using GuardianFx = std::array<GuardianFxAssets, 6>;
// Texture keys for the three element atlases in `dialogue_textures`.
inline int guardian_fx_key(int atlas, int frame) { return 0x40000 + atlas * 256 + frame; }

struct HeroVisualAssets {
    SpriteTextures textures;
    std::array<CharacterSpriteTable, 6> tables;
    std::array<bool, 6> mirror_on_load{};
    int element{};   // bu.a(...,p5): guardian element the weapon glow was tinted for
    std::array<EifAnimation, 3> magic{}; // bu.a(br,4,10): Kail's c3/s/ea2..ea4 drawn from the e atlas
    bool peaceful{}; // !ae.c: map kinds 1/5/9/15 hide frame layers 6 and 7 (weapon)
};

void destroy_textures(SpriteTextures& textures);
SDL_Texture* create_image_texture(SDL_Renderer* renderer, const DecodedImage& image);
SpriteTextures load_sprite_textures(SDL_Renderer* renderer, const GameArchive& archive,
                                    const std::string& stem,
                                    const std::unordered_set<std::size_t>& frame_ids,
                                    const std::vector<std::pair<std::uint32_t,std::uint32_t>>& recolors = {});
void ensure_sprite_texture(SDL_Renderer* renderer, const GameArchive& archive,
                           const std::string& stem, int frame_id, SpriteTextures& cache);
SpriteTextures load_terrain_textures(SDL_Renderer* renderer, const GameArchive& archive, const GameMap& map);
SpriteTextures load_object_textures(SDL_Renderer* renderer, const GameArchive& archive,
                                    const GameMap& map, const EventGrid& event,
                                    const std::vector<MapEntity>& objects);
NpcVisualAssets load_npc_textures(SDL_Renderer* renderer, const GameArchive& archive,
                                  const std::vector<MapEntity>& npcs);
void load_enemy_textures(SDL_Renderer* renderer, const GameArchive& archive,
                         const std::vector<MapEntity>& enemies, NpcVisualAssets& assets);
HeroVisualAssets load_hero_textures(SDL_Renderer* renderer, const GameArchive& archive,
                                    int character_class, const EquipmentSet& equipment, int element = 0);
// Boss encounters share one /boss/{n} atlas keyed by their first stats id (ce.e(byte)).
inline int boss_texture_base(int stats_id) {
    return stats_id >= 42 ? 42 : stats_id >= 39 ? 39 : stats_id >= 35 ? 35 : 32;
}
