#pragma once
// One frame: the field (terrain, objects, NPCs, enemies, hero, effects), the interface over it and,
// on 3DS, the bottom screen.
#include <SDL.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "combat.hpp"
#include "eif.hpp"
#include "event_visual.hpp"
#include "game_assets.hpp"
#include "hero_movement.hpp"
#include "map.hpp"
#include "npc_movement.hpp"
#include "visual_ui.hpp"

// Port smoothing: logic stays on its ticks; between two ticks the renderer draws NPCs and
// enemies this many logical pixels away from their current tick position (towards the last one).
extern std::vector<SDL_Point> g_npc_draw_offsets, g_enemy_draw_offsets;
// as.paint pairs one logic step with one ac.draw; extra smooth renders must not advance NPC frames.
extern std::uint32_t g_logic_tick;
void render(SDL_Renderer* renderer, const GameMap& map, const EventGrid& event,
            const std::vector<MapEntity>& objects,
            const std::vector<MapEntity>& npcs,
            const std::vector<MapEntity>& enemies,
            std::vector<NpcMotionState>& npc_motion,
            const HeroState& hero, bool show_collision,
            const SpriteTextures& terrain_textures,
            const SpriteTextures& object_textures,
            const NpcVisualAssets& npc_assets,
            const HeroVisualAssets& hero_assets,
            const EventVisualState& event_visuals,
            const VisualUiModel& ui_model,
            SDL_Texture* dialogue_font_texture,
            const BitmapFont& dialogue_font,
            const SpriteTextures& dialogue_textures,
            const combat::CombatWorld& combat_world,
            const SpriteTextures& combat_textures,
            const GuardianFx& guardian_fx,
            const std::vector<std::string>& guardian_skill_names,
            const std::array<EifAnimation,4>& death_effects); // [3] = char/lvup.eif
