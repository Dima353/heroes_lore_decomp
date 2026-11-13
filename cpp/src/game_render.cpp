#include <SDL.h>
#include "gpu_3ds.hpp"
#include "game_render.hpp"
#include "platform.hpp"
#include "startup_log.hpp"
#include "world_presentation.hpp"
#include "event_trigger.hpp"
#include "viewport.hpp"
#if HEROES_LORE_3DS
#include "platform_3ds_screen.hpp"
#endif
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>

std::vector<SDL_Point> g_npc_draw_offsets, g_enemy_draw_offsets;
std::uint32_t g_logic_tick = 0;
static SDL_Point draw_offset(const std::vector<SDL_Point>& offsets, std::size_t index) {
    return index < offsets.size() ? offsets[index] : SDL_Point{0, 0};
}

static const std::vector<NpcSpriteLayer>* live_enemy_frame(const NpcVisualAssets& assets,
                                                          const combat::Enemy& enemy) {
    if (enemy.boss != combat::BossKind::none) {
        // av.a(Graphics): walk +4, attack +12 (cc: its current set), death +8, else idle.
        const auto boss = assets.boss_animations.find(enemy.sprite);
        if (boss == assets.boss_animations.end()) return nullptr;
        const bool attacking = enemy.state == combat::EnemyState::attack;
        int direction = enemy.direction;
        if (enemy.draw_direction && !(enemy.boss == combat::BossKind::ak && attacking)) direction = enemy.draw_direction;
        if (direction < 1 || direction > 4) return nullptr;
        std::size_t type = enemy.state == combat::EnemyState::walk ? 1
            : enemy.state == combat::EnemyState::dying ? 2 : attacking ? 3 : 0;
        if (attacking && enemy.boss == combat::BossKind::cc) type = std::size_t(2 + std::clamp(enemy.pattern, 1, 3));
        const auto* frames = &boss->second.types[type][std::size_t(direction - 1)];
        if (frames->empty()) frames = &boss->second.types[0][std::size_t(direction - 1)];
        if (frames->empty()) return nullptr;
        const auto frame = std::size_t(std::max(0, enemy.frame));
        return &(*frames)[type == 2 ? std::min(frame, frames->size() - 1) : frame % frames->size()];
    }
    // al.a(Graphics): state 2 walk (type 1), 3 attack (type 2), otherwise idle (type 0).
    const auto found = assets.enemy_animations.find(enemy.sprite);
    if (found == assets.enemy_animations.end() || enemy.direction < 1 || enemy.direction > 4) return nullptr;
    const auto& set = enemy.state == combat::EnemyState::walk ? found->second.walk
        : enemy.state == combat::EnemyState::attack ? found->second.attack : found->second.idle;
    const auto& frames = set[std::size_t(enemy.direction - 1)];
    if (frames.empty()) return nullptr;
    return &frames[std::size_t(std::max(0, enemy.frame)) % frames.size()];
}

// ce.u drawn TOP|HCENTER at (x, y-3): the ground shadow of heroes, NPCs and one-tile enemies.
static void draw_ground_shadow(SDL_Renderer* renderer, const SpriteTextures& combat_textures,
                               int left, int top, int pixel_scale, int x, int y) {
    const auto shadow = combat_textures.find(3);
    int w = 0, h = 0;
    if (shadow == combat_textures.end() || SDL_QueryTexture(shadow->second, nullptr, nullptr, &w, &h) != 0) return;
    SDL_Rect dest{left + (x - w / 2) * pixel_scale, top + (y - 3) * pixel_scale, w * pixel_scale, h * pixel_scale};
    SDL_RenderCopy(renderer, shadow->second, nullptr, &dest);
}

// fillArc(x-11, y-6, 22, 9, 0, 360) in 0x1F3F3F: the shadow of two-tile enemies.
static void draw_oval_shadow(SDL_Renderer* renderer, int left, int top, int pixel_scale, int x, int y) {
    SDL_SetRenderDrawColor(renderer, 0x1F, 0x3F, 0x3F, 255);
    for (int row = 0; row < 9; ++row) {
        const double v = (row + 0.5 - 4.5) / 4.5;
        const int half = int(11.0 * std::sqrt(std::max(0.0, 1.0 - v * v)) + 0.5);
        if (half <= 0) continue;
        SDL_Rect span{left + (x - half) * pixel_scale, top + (y - 6 + row) * pixel_scale, 2 * half * pixel_scale, pixel_scale};
        SDL_RenderFillRect(renderer, &span);
    }
}

static void draw_enemies(SDL_Renderer* renderer, const std::vector<MapEntity>& enemies,
                         const GameMap& map, const NpcVisualAssets& assets,
                         int left, int top, int cell, int pixel_scale,
                         const combat::CombatWorld* combat_world = nullptr,
                         const SpriteTextures* combat_textures = nullptr) {
    SDL_SetRenderDrawColor(renderer, 235, 70, 70, 255);
    for (const auto& enemy : enemies) {
        if (enemy.resource_slot < 0 || enemy.x >= map.width || enemy.y >= map.height) continue;
        const int id = enemy.code >= 128 ? int(enemy.code) - 256 : enemy.code;
        if (combat_world && enemy.frame_index >= 0 &&
            std::size_t(enemy.frame_index) < combat_world->enemies().size()) {
            const auto& live = combat_world->enemies()[std::size_t(enemy.frame_index)];
            if (live.hidden) continue; // j.d ambushers stay invisible until provoked
            const auto smooth = draw_offset(g_enemy_draw_offsets, std::size_t(enemy.frame_index));
            int x = live.x_px + smooth.x + 8 + (live.width - 1) * 8, y = live.y_px + smooth.y + 8 + live.draw_offset_y;
            const int texture_id = live.boss != combat::BossKind::none ? boss_texture_base(live.sprite) : id;
            if (live.shake == 3 || live.shake == 1) {
                x += combat::kDx[std::size_t(live.shake_direction)] * 3;
                y += combat::kDy[std::size_t(live.shake_direction)] * 3;
            }
            // al.a(Graphics,II): ordinary enemies except m 16/22, plus the cc boss, get a shadow
            // at the shaken anchor: ce.u when one tile wide, an oval otherwise.
            if ((live.boss == combat::BossKind::none && live.sprite != 16 && live.sprite != 22) ||
                live.boss == combat::BossKind::cc) {
                if (live.width != 1) draw_oval_shadow(renderer, left, top, pixel_scale, x, y);
                else if (combat_textures) draw_ground_shadow(renderer, *combat_textures, left, top, pixel_scale, x, y);
            }
            SDL_SetRenderDrawColor(renderer, 235, 70, 70, 255);
            if (const auto* frame = live_enemy_frame(assets, live)) {
                for (const auto& layer : *frame) {
                    const auto texture = assets.enemy_textures.find((texture_id << 8) | layer.frame);
                    if (texture == assets.enemy_textures.end()) continue;
                    int width = 0, height = 0;
                    if (SDL_QueryTexture(texture->second, nullptr, nullptr, &width, &height) != 0) continue;
                    SDL_Rect target{left + (x + layer.x) * pixel_scale, top + (y + layer.y) * pixel_scale,
                                    width * pixel_scale, height * pixel_scale};
                    SDL_RenderCopyEx(renderer, texture->second, nullptr, &target, 0.0, nullptr,
                                     layer.mirrored ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
                }
                continue;
            }
        }
        const auto animation = assets.enemy_idle_frames.find(id);
        bool drawn = false;
        if (animation != assets.enemy_idle_frames.end()) {
            for (const auto& layer : animation->second) {
                const auto key = (id << 8) | layer.frame;
                const auto texture = assets.enemy_textures.find(key);
                if (texture == assets.enemy_textures.end()) continue;
                int width = 0, height = 0;
                if (SDL_QueryTexture(texture->second, nullptr, nullptr, &width, &height) != 0) continue;
                SDL_Rect target{left + enemy.x * cell + 8 * pixel_scale + layer.x * pixel_scale,
                                top + enemy.y * cell + 8 * pixel_scale + layer.y * pixel_scale,
                                width * pixel_scale, height * pixel_scale};
                SDL_RenderCopyEx(renderer, texture->second, nullptr, &target, 0.0, nullptr,
                                 layer.mirrored ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
                drawn = true;
            }
        }
        if (!drawn) {
            SDL_Rect marker{left + enemy.x * cell + cell / 4, top + enemy.y * cell + cell / 4,
                            std::max(1, cell / 2), std::max(1, cell / 2)};
            SDL_RenderFillRect(renderer, &marker);
        }
    }
}

static bool draw_hero_sprite(SDL_Renderer* renderer, const HeroState& hero,
                             const HeroVisualAssets& assets, int left, int top,
                             int pixel_scale, int combat_state = -1, int combat_frame = 0) {
    if (hero.direction < 1 || hero.direction > 4) return false;
    // ao.a(Graphics): attack combos use sprite states 3..10, death uses state 2 facing up.
    const std::uint8_t animation_state = combat_state >= 0 ? std::uint8_t(combat_state)
        : hero.motion_mode == 2 ? 1 : 0;
    const auto direction = static_cast<std::uint8_t>(combat_state == 2 ? 0 : hero.direction - 1);
    const int animation_tick = combat_state >= 0 ? combat_frame
        : hero.movement_marker < 0 ? 0 : hero.movement_marker;
    const int anchor_x = left + (hero.x_pixels + 8) * pixel_scale;
    const int anchor_y = top + (hero.y_pixels + 8) * pixel_scale;
    bool drawn = false;

    for (std::uint8_t slot = 0; slot < 9; ++slot) {
        if (assets.peaceful && (slot == 6 || slot == 7)) continue; // ao.a: weapon layers need ae.c
        for (std::size_t category = 0; category < assets.tables.size(); ++category) {
            const auto* record = find_character_sprite_record(
                assets.tables[category], slot, animation_state, direction);
            if (!record || record->frames.empty()) continue;
            const auto frame_index = static_cast<std::size_t>(animation_tick) % record->frames.size();
            for (const auto& layer : record->frames[frame_index]) {
                if (layer.atlas_frame == 0xff) continue;
                const auto key = (static_cast<int>(category) << 8) | layer.atlas_frame;
                const auto texture = assets.textures.find(key);
                if (texture == assets.textures.end()) continue;
                int width = 0, height = 0;
                if (SDL_QueryTexture(texture->second, nullptr, nullptr, &width, &height) != 0) continue;
                SDL_Rect target{anchor_x + layer.x * pixel_scale,
                                anchor_y + layer.y * pixel_scale,
                                width * pixel_scale, height * pixel_scale};
                const auto flip = layer.mirrored && assets.mirror_on_load[category]
                    ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE;
                SDL_RenderCopyEx(renderer, texture->second, nullptr, &target, 0.0, nullptr, flip);
                drawn = true;
            }
        }
    }
    return drawn;
}

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
                   const std::array<EifAnimation,4>& death_effects) { // [3] = char/lvup.eif
#if HEROES_LORE_3DS
    Uint32 phase_mark=SDL_GetTicks();
    // SDL scans the 3DS buttons once per SDL_PumpEvents and only reports changes between two scans,
    // so a tap shorter than a slow frame was lost; pumping between the phases samples 4 more times.
    // Only HID and the joystick: a full SDL_PumpEvents also runs aptMainLoop, an IPC round trip to the
    // system core that the audio thread keeps busy (once a frame, in the event loop, is enough).
    const auto phase=[&](int index){hidScanInput();SDL_JoystickUpdate();const Uint32 now=SDL_GetTicks();g_3ds_phase_ms[index]+=now-phase_mark;phase_mark=now;};
#endif
#if HEROES_LORE_3DS
    // The title menus are drawn on the bottom screen; the top keeps the logo page with its feathers
    // (Help / Options opened in game: the field).
    const bool title_below=ui_model.screen==VisualScreen::title_menu && ui_model.startup.page!=TitlePage::splash &&
        ui_model.startup.page!=TitlePage::logo && ui_model.startup.page!=TitlePage::loading;
    const bool field_under_title=title_below && ui_model.startup.in_game;
#else
    constexpr bool field_under_title=false;
#endif
    int output_w = 0, output_h = 0;
    SDL_GetRendererOutputSize(renderer, &output_w, &output_h);
    static bool logged_output_size = false;
    if (!logged_output_size) {
        startup_log("renderer output size: " + std::to_string(output_w) + "x" + std::to_string(output_h));
        logged_output_size = true;
    }
    SDL_SetRenderDrawColor(renderer, 16, 18, 26, 255);
    SDL_RenderClear(renderer);
    if (ui_model.game_over_chars >= 0) {
        // as.b(Graphics): black screen, the death pose at (cx, cy+20), ce.g[32] revealed two
        // characters per draw at (cx - width/2, cy-20), grey shadow at +1 then white.
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        const int scale = std::max(1, std::min(output_w / 240, output_h / 240));
        const int cx = output_w / 2, cy = output_h / 2;
        HeroState pose; pose.direction = 1;
        draw_hero_sprite(renderer, pose, hero_assets, cx - 8 * scale, cy + (20 - 8) * scale, scale, 2, 0);
        if (dialogue_font_texture) {
            const auto glyphs = layout_bitmap_text(dialogue_font, ui_model.game_over_text, 0, 0, 100000, scale);
            int width = 0;
            for (const auto& glyph : glyphs) width = std::max(width, glyph.x + int(glyph.source_width) * scale);
            const auto pass = [&](int dx, int dy, Uint8 shade) {
                SDL_SetTextureColorMod(dialogue_font_texture, shade, shade, shade);
                int shown = 0;
                for (const auto& glyph : glyphs) {
                    if (shown++ >= ui_model.game_over_chars) break;
                    SDL_Rect src{int(glyph.source_x), 0, int(glyph.source_width), int(dialogue_font.height)};
                    SDL_Rect dest{cx - width / 2 + glyph.x + dx * scale, cy - 20 * scale + glyph.y + dy * scale,
                                  int(glyph.source_width) * scale, int(dialogue_font.height) * scale};
                    SDL_RenderCopy(renderer, dialogue_font_texture, &src, &dest);
                }
            };
            pass(1, 1, 0x7F);
            pass(0, 0, 0xFF);
            SDL_SetTextureColorMod(dialogue_font_texture, 255, 255, 255);
        }
    }
    if((ui_model.screen!=VisualScreen::title_menu || field_under_title) && ui_model.game_over_chars<0) {
    const auto viewport = ui_model.world_viewport.value_or(calculate_world_viewport(output_w,output_h,map.width,map.height,
                                                    hero.x_pixels,hero.y_pixels,
                                                    21*std::max(1,std::min(output_w/320,output_h/240))));
    const int cell = viewport.cell;
    const auto shake = combat_world.screen_shake();
    const int left = viewport.left + (shake.first+event_visuals.shake_x) * viewport.scale;
    const int top = viewport.top + (shake.second+event_visuals.shake_y) * viewport.scale;
    HL3D_DEPTH(-1.0f); // stereo 3D: the field is a ground plane tilted away towards the top of the screen
    for (int y = 0; y < map.height; ++y) {
        if (top+(y+1)*cell <= 0 || top+y*cell >= output_h) continue;
        for (int x = 0; x < map.width; ++x) {
            if (left+(x+1)*cell <= 0 || left+x*cell >= output_w) continue;
            const auto value = map.terrain[static_cast<std::size_t>(y) * map.width + x];
            SDL_Rect rect{left + x * cell, top + y * cell, cell, cell};
            const auto texture = terrain_textures.find(value);
            if (texture != terrain_textures.end())
                SDL_RenderCopy(renderer, texture->second, nullptr, &rect);
            const auto collision = event.overlay[static_cast<std::size_t>(y) * map.width + x];
            if (show_collision && collision >= 128) {
                SDL_SetRenderDrawColor(renderer, 220, 50, 45, 255);
                SDL_Rect mark{rect.x + cell / 4, rect.y + cell / 4,
                              std::max(1, cell / 2), std::max(1, cell / 2)};
                SDL_RenderFillRect(renderer, &mark);
            }
        }
    }
    const int pixel_scale = std::max(1, cell / 16);
    HL3D_DEPTH(-1.0f); // objects, characters and effects stand on it at their feet
    const auto draw_object = [&](std::size_t i) {
        if (i < event.object_visible.size() && !event.object_visible[i]) return;
        const auto& object = objects[i];
        const auto texture = object_textures.find(object.frame_index);
        if (texture == object_textures.end()) return;
        int width = 0, height = 0;
        if (SDL_QueryTexture(texture->second, nullptr, nullptr, &width, &height) != 0) return;
        const int draw_w = width * pixel_scale;
        const int draw_h = height * pixel_scale;
        const int anchor_x = left + object.x * cell + object.offset_x * pixel_scale;
        const int anchor_y = top + object.y * cell + object.offset_y * pixel_scale;
        SDL_Rect target{anchor_x - draw_w / 2, anchor_y - draw_h, draw_w, draw_h};
        SDL_RenderCopy(renderer, texture->second, nullptr, &target);
    };
    SDL_SetRenderDrawColor(renderer, 60, 220, 230, 255);
    const auto draw_npc = [&](std::size_t i) {
        if (i < event.npc_visible.size() && !event.npc_visible[i]) return;
        const auto& npc = npcs[i];
        if (npc.x >= map.width || npc.y >= map.height) return;
        bool drawn = false;
        {   // ac.a(Graphics,II): ce.u under every visible NPC, static or animated
            const auto smooth = draw_offset(g_npc_draw_offsets, i);
            int lift = 0;
            // Port: a wide static image is a body lying on the ground; the original keeps the shadow
            // at the image's bottom edge, which makes it float. Centre the shadow under the body.
            if (npc.frame_index >= 0) {
                const auto image = npc_assets.textures.find(0x10000 + npc.frame_index);
                int w = 0, h = 0;
                if (image != npc_assets.textures.end() && SDL_QueryTexture(image->second, nullptr, nullptr, &w, &h) == 0 && w > h)
                    lift = h / 2;
            }
            draw_ground_shadow(renderer, combat_textures, left, top, pixel_scale,
                               npc.x * 16 + 8 + (npc.partial_x ? 8 : 0) + smooth.x,
                               npc.y * 16 + 8 + (npc.partial_y ? 8 : 0) + smooth.y - lift);
        }
        if (npc.frame_index >= 0) {
            const auto texture = npc_assets.textures.find(0x10000 + npc.frame_index);
            if (texture != npc_assets.textures.end()) {
                int width = 0, height = 0;
                if (SDL_QueryTexture(texture->second, nullptr, nullptr, &width, &height) == 0) {
                    const int draw_w = width * pixel_scale;
                    const int draw_h = height * pixel_scale;
                    const auto smooth = draw_offset(g_npc_draw_offsets, i);
                    const int anchor_x = left + npc.x * cell + (8 + (npc.partial_x ? 8 : 0) + smooth.x) * pixel_scale;
                    const int anchor_y = top + npc.y * cell + (8 + (npc.partial_y ? 8 : 0) + smooth.y) * pixel_scale;
                    SDL_Rect target{anchor_x - draw_w / 2, anchor_y - draw_h, draw_w, draw_h};
                    SDL_RenderCopy(renderer, texture->second, nullptr, &target);
                    drawn = true;
                }
            }
        } else {
            const int code = npc.code >= 128 ? int(npc.code) - 256 : npc.code;
            const auto animation = npc_assets.animations.find(code);
            if (animation != npc_assets.animations.end()) {
                int direction = i < npc_motion.size() ? npc_motion[i].direction : 2;
                if (direction < 1 || direction > 4) direction = 2;
                const auto slot = static_cast<std::size_t>(direction - 1);
                const bool walking = i < npc_motion.size() && npc_motion[i].motion_mode == 2;
                const auto& sequence = walking ? animation->second.walk[slot]
                                               : animation->second.idle[slot];
                const auto& idle_sequence = animation->second.idle[slot];
                const auto& selected = sequence.empty() ? idle_sequence : sequence;
                if (!selected.empty()) {
                    const auto frame_index = i < npc_motion.size()
                        ? npc_motion[i].animation_frame % selected.size() : 0;
                    const auto& layers = selected[frame_index];
                    for (const auto& layer : layers) {
                        const auto key = (code << 8) | layer.frame;
                        const auto texture = npc_assets.textures.find(key);
                        if (texture == npc_assets.textures.end()) continue;
                        int width = 0, height = 0;
                        if (SDL_QueryTexture(texture->second, nullptr, nullptr, &width, &height) != 0) continue;
                        const int draw_w = width * pixel_scale;
                        const int draw_h = height * pixel_scale;
                        const auto smooth = draw_offset(g_npc_draw_offsets, i);
                        const int x = left + npc.x * cell + (8 + (npc.partial_x ? 8 : 0) + smooth.x) * pixel_scale + layer.x * pixel_scale;
                        const int y = top + npc.y * cell + (8 + (npc.partial_y ? 8 : 0) + smooth.y) * pixel_scale + layer.y * pixel_scale;
                        SDL_Rect target{x, y, draw_w, draw_h};
                        SDL_RenderCopyEx(renderer, texture->second, nullptr, &target, 0.0, nullptr,
                                         layer.mirrored ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
                        drawn = true;
                    }
                    if (i < npc_motion.size() && selected.size() > 1 && npc_motion[i].animation_tick != g_logic_tick) {
                        npc_motion[i].animation_frame = static_cast<std::uint8_t>((frame_index + 1) % selected.size());
                        npc_motion[i].animation_tick = g_logic_tick;
                    }
                }
            }
        }
        if (drawn) return;
        SDL_Rect marker{left + npc.x * cell + cell / 4,
                        top + npc.y * cell + cell / 4,
                        std::max(1, cell / 2), std::max(1, cell / 2)};
        SDL_RenderFillRect(renderer, &marker);
    };
    const auto draw_hero = [&]() {
        const auto hit_offset=combat_world.hero_hit_offset();
        // ao.a(Graphics,II): ce.u before the hero layers.
        draw_ground_shadow(renderer, combat_textures, left + hit_offset.first * pixel_scale, top + hit_offset.second * pixel_scale,
                           pixel_scale, hero.x_pixels + 8, hero.y_pixels + 8);
        if (!draw_hero_sprite(renderer, hero, hero_assets, left+hit_offset.first*pixel_scale, top+hit_offset.second*pixel_scale, pixel_scale,
                              combat_world.hero_sprite_state(), combat_world.hero_frame())) {
            const int hero_center_x = left + (hero.x_pixels + 8) * pixel_scale;
            const int hero_center_y = top + (hero.y_pixels + 8) * pixel_scale;
            const int hero_size = std::max(6, 8 * pixel_scale);
            SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
            SDL_Rect hero_outline{hero_center_x - hero_size / 2 - 1,
                                  hero_center_y - hero_size / 2 - 1,
                                  hero_size + 2, hero_size + 2};
            SDL_RenderFillRect(renderer, &hero_outline);
            SDL_SetRenderDrawColor(renderer, 70, 255, 100, 255);
            SDL_Rect hero_body{hero_center_x - hero_size / 2, hero_center_y - hero_size / 2,
                               hero_size, hero_size};
            SDL_RenderFillRect(renderer, &hero_body);
            int direction_x = 0, direction_y = 0;
            if (hero.direction == 1) direction_y = -1;
            else if (hero.direction == 2) direction_y = 1;
            else if (hero.direction == 3) direction_x = -1;
            else if (hero.direction == 4) direction_x = 1;
            SDL_RenderDrawLine(renderer, hero_center_x, hero_center_y,
                               hero_center_x + direction_x * 5 * pixel_scale,
                               hero_center_y + direction_y * 5 * pixel_scale);
        } else if (const int dash = combat_world.hero_dash_offset(); dash > 0 && hero.direction >= 1 && hero.direction <= 4) {
            // ao.a(Graphics,II) @193-244: after the sword dash the attack frame is drawn again
            // 16*y pixels behind the hero (u.g = opposite direction), where the dash started.
            const int back_x = -combat::kDx[std::size_t(hero.direction)] * 16 * dash;
            const int back_y = -combat::kDy[std::size_t(hero.direction)] * 16 * dash;
            draw_hero_sprite(renderer, hero, hero_assets, left + (hit_offset.first + back_x) * pixel_scale,
                             top + (hit_offset.second + back_y) * pixel_scale, pixel_scale,
                             combat_world.hero_sprite_state(), combat_world.hero_frame());
        }
    };
    // ae.a(Graphics,int,int): ground drops, etcui 10 gold / 9 item, blinking for the last 16 ticks.
    for (const auto& drop : combat_world.drops()) {
        if (drop.ttl <= 16 && drop.ttl % 3 == 0) continue;
        const auto texture = combat_textures.find(drop.type == -1 ? 10 : 9);
        int w = 0, h = 0;
        if (texture == combat_textures.end() || SDL_QueryTexture(texture->second, nullptr, nullptr, &w, &h) != 0) continue;
        SDL_Rect dest{left + (drop.x * 16 + 8 - w / 2) * pixel_scale, top + (drop.y * 16 + 8 - h) * pixel_scale,
                      w * pixel_scale, h * pixel_scale};
        SDL_RenderCopy(renderer, texture->second, nullptr, &dest);
    }
    // i/y: projectile frames drawn TOP|LEFT from the tile anchor (+8,+9), atlas of their owner.
    const auto draw_projectile = [&](const combat::Projectile& p) {
        if (p.from_hero) { // ao.d: Kail's magic, ce.b[q-2] over the e atlas (variant 1 = mirrored image)
            if (p.effect < 0 || p.effect > 2) return;
            const auto& magic = hero_assets.magic[std::size_t(p.effect)];
            const int frame = std::max(0, p.frame - 1);
            if (std::size_t(frame) >= magic.size()) return;
            int x = p.x * 16 + 8, y = p.y * 16 + 9;
            // i.a(Graphics,II): a two-frame effect moves half a tile on its second frame.
            if (p.duration == 2 && frame == 1) { x += combat::kDx[std::size_t(p.direction)] * 8; y += combat::kDy[std::size_t(p.direction)] * 8; }
            for (const auto& layer : magic[std::size_t(frame)]) {
                const auto texture = hero_assets.textures.find((5 << 8) | layer.atlas_frame);
                int w = 0, h = 0;
                if (texture == hero_assets.textures.end() || SDL_QueryTexture(texture->second, nullptr, nullptr, &w, &h) != 0) continue;
                SDL_Rect dest{left + (x + layer.x) * pixel_scale, top + (y + layer.y) * pixel_scale, w * pixel_scale, h * pixel_scale};
                SDL_RenderCopyEx(renderer, texture->second, nullptr, &dest, 0.0, nullptr, layer.variant ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
            }
            return;
        }
        const auto effect = npc_assets.effects.find(p.effect);
        if (effect == npc_assets.effects.end() || effect->second.empty()) return;
        const int frame = std::max(0, p.frame - 1);
        if (std::size_t(frame) >= effect->second.size()) return;
        int x = p.x * 16 + 8, y = p.y * 16 + 9;
        if (p.duration == 2 && frame == 1) { x += combat::kDx[std::size_t(p.direction)] * 8; y += combat::kDy[std::size_t(p.direction)] * 8; }
        const int base = p.effect >= 32 ? boss_texture_base(p.effect) : p.effect;
        for (const auto& layer : effect->second[std::size_t(frame)]) {
            if (layer.atlas_frame == 0xff) continue;
            const auto texture = npc_assets.enemy_textures.find((base << 8) | layer.atlas_frame);
            int w = 0, h = 0;
            if (texture == npc_assets.enemy_textures.end() || SDL_QueryTexture(texture->second, nullptr, nullptr, &w, &h) != 0) continue;
            SDL_Rect dest{left + (x + layer.x) * pixel_scale, top + (y + layer.y) * pixel_scale, w * pixel_scale, h * pixel_scale};
            SDL_RenderCopy(renderer, texture->second, nullptr, &dest);
        }
    };
    const auto& g = combat_world.guardian();
    const GuardianFxAssets* fx = g.id >= 0 && g.id < 6 ? &guardian_fx[std::size_t(g.id)] : nullptr;
    // Element atlas image n (ce.a[12][n]) drawn BOTTOM|HCENTER at a world pixel anchor.
    const auto draw_atlas = [&](int frame, int wx, int wy) {
        if (!fx) return;
        const auto texture = dialogue_textures.find(guardian_fx_key(fx->atlas, frame));
        int w = 0, h = 0;
        if (texture == dialogue_textures.end() || SDL_QueryTexture(texture->second, nullptr, nullptr, &w, &h) != 0) return;
        SDL_Rect dest{left + wx * pixel_scale - w * pixel_scale / 2, top + (wy - h) * pixel_scale, w * pixel_scale, h * pixel_scale};
        SDL_RenderCopy(renderer, texture->second, nullptr, &dest);
    };
    // as.b: one .eif frame, layers TOP|LEFT from the anchor, images from the element atlas.
    const auto draw_eif = [&](const EifAnimation& eif, int frame, int wx, int wy) {
        if (!fx || frame < 0 || std::size_t(frame) >= eif.size()) return;
        for (const auto& layer : eif[std::size_t(frame)]) {
            if (layer.atlas_frame == 0xff) continue;
            const auto texture = dialogue_textures.find(guardian_fx_key(fx->atlas, layer.atlas_frame));
            int w = 0, h = 0;
            if (texture == dialogue_textures.end() || SDL_QueryTexture(texture->second, nullptr, nullptr, &w, &h) != 0) continue;
            SDL_Rect dest{left + (wx + layer.x) * pixel_scale, top + (wy + layer.y) * pixel_scale, w * pixel_scale, h * pixel_scale};
            SDL_RenderCopy(renderer, texture->second, nullptr, &dest);
        }
    };
    // y: effects spawned by guardian skills, anchored at tile pixel + (8, 9).
    const auto draw_world_effect = [&](const combat::WorldEffect& e) {
        if (!fx) return;
        const int x = e.x_px + 8, y = e.y_px + 9;
        static constexpr int slot_of_kind[11]{-1, 0, -1, -1, 0, 0, 1, 0, 1, 1, -1};
        switch (e.kind) {
            case 2: draw_eif(fx->extra, e.frame, x, y); break;
            case 4: case 5: case 7: {
                if (e.frame >= 3) { draw_eif(fx->slots[0], e.frame - 3, x, y); break; }
                const auto texture = dialogue_textures.find(guardian_fx_key(fx->atlas, e.kind == 4 ? 8 : 11));
                int w = 0, h = 0;
                if (texture == dialogue_textures.end() || SDL_QueryTexture(texture->second, nullptr, nullptr, &w, &h) != 0) break;
                const int sink = h * (e.frame == 0 ? 7 : e.frame == 1 ? 5 : 3) / 10, visible = h - sink;
                SDL_Rect src{0, 0, w, visible};
                SDL_Rect dest{left + x * pixel_scale - w * pixel_scale / 2, top + (y - visible) * pixel_scale, w * pixel_scale, visible * pixel_scale};
                SDL_RenderCopy(renderer, texture->second, &src, &dest);
                break;
            }
            case 10: // y.d: atlas 0 at the anchor, atlas 1 three pixels lower
                if (e.frame <= 1) draw_atlas(0, x, y);
                if (e.frame >= 1) draw_atlas(1, x, y + 3);
                break;
            default:
                if (slot_of_kind[e.kind] >= 0) draw_eif(fx->slots[std::size_t(slot_of_kind[e.kind])], e.frame, x, y);
                break;
        }
    };
    // bj: auras on the hero or on marked enemies.
    const auto draw_aura = [&](const combat::Aura& a) {
        if (a.delay > 0 || a.guardian < 0 || a.guardian > 5) return;
        const auto& afx = guardian_fx[std::size_t(a.guardian)];
        int x = hero.x_pixels + 8, y = hero.y_pixels + 8;
        if (a.target >= 0) {
            if (std::size_t(a.target) >= combat_world.enemies().size()) return;
            const auto& enemy = combat_world.enemies()[std::size_t(a.target)];
            const auto smooth = draw_offset(g_enemy_draw_offsets, std::size_t(a.target));
            x = enemy.x_px + smooth.x + 8 + (enemy.width - 1) * 8; y = enemy.y_px + smooth.y + 8; // al.draw anchor
        }
        const auto saved = fx;
        fx = &afx;
        if (a.guardian == 0 && a.skill == 0) {
            static constexpr int frames[6]{7, 8, 7, 8, 9, 9};
            if (a.frame < 6) draw_atlas(frames[a.frame], x, y - std::min(a.frame, 4));
        } else if (a.skill == 2 && a.guardian <= 1) {
            const int phase = a.frame % 4;
            if (phase == 1 || phase == 2) draw_atlas(0, x, y + 9);
            if (phase == 2 || phase == 3) draw_atlas(1, x, y + 12);
        } else draw_eif(afx.slots[std::size_t(std::clamp(a.skill, 0, 2))], a.frame, x, y);
        fx = saved;
    };
    const auto draw_guardian = [&]() {
        if (g.state == 0 || g.id < 0) return;
        const auto frame0 = dialogue_textures.find(720 + g.id * 2), frame1 = dialogue_textures.find(720 + g.id * 2 + 1);
        const int wx = g.x * 16 + 8, wy = g.y * 16 + 8;
        const int ax = left + wx * pixel_scale, ay = top + wy * pixel_scale;
        // Draw `texture` BOTTOM|HCENTER at the anchor, pushed down by `sink` tenths and clipped at the anchor.
        const auto draw = [&](decltype(frame0) texture, int sink_tenths, int dy) {
            int w = 0, h = 0;
            if (texture == dialogue_textures.end() || SDL_QueryTexture(texture->second, nullptr, nullptr, &w, &h) != 0) return;
            const int visible = h - h * sink_tenths / 10;
            if (visible <= 0) return;
            SDL_Rect src{0, 0, w, visible};
            SDL_Rect dest{ax - w * pixel_scale / 2, ay - visible * pixel_scale + dy * pixel_scale, w * pixel_scale, visible * pixel_scale};
            SDL_RenderCopy(renderer, texture->second, &src, &dest);
        };
        // p.a(Graphics): summon circle pieces (atlas 0..3), rising/sinking figure, idle bob.
        if (g.state == 1) {
            switch (g.anim) {
                case 0: draw_atlas(2, wx, wy); break;
                case 1: draw_atlas(3, wx, wy); break;
                case 2: draw_atlas(3, wx, wy); draw_atlas(0, wx, wy + 3); break;
                case 3: draw_atlas(3, wx, wy); draw_atlas(0, wx, wy + 3); draw_atlas(1, wx, wy + 6); break;
                case 4: draw_atlas(2, wx, wy); draw_atlas(1, wx, wy + 6); break;
                case 5: draw(frame0, 7, 0); break;
                case 6: draw(frame0, 5, 0); draw_atlas(2, wx, wy); break;
                case 7: draw(frame0, 3, 0); draw_atlas(3, wx, wy); break;
                case 8: draw(frame0, 2, 0); draw_atlas(3, wx, wy); break;
                case 9: draw(frame0, 0, 0); draw_atlas(2, wx, wy); break;
                default: break;
            }
        } else if (g.state == 2) {
            draw(frame1, 0, g.loops % 3 == 0 ? 1 : 0);
            if (g.anim == 1 || g.anim == 2) draw_atlas(0, wx, wy + 3);
            if (g.anim == 2 || g.anim == 3) draw_atlas(1, wx, wy + 6);
        } else {
            if (g.anim < 6) draw(frame1, 0, 0);
            switch (g.anim) {
                case 6: draw(frame0, 1, 0); draw_atlas(3, wx, wy); draw_atlas(0, wx, wy + 3); draw_atlas(1, wx, wy + 6); break;
                case 7: draw(frame0, 3, 0); draw_atlas(3, wx, wy); draw_atlas(0, wx, wy + 3); break;
                case 8: draw(frame0, 5, 0); draw_atlas(3, wx, wy); break;
                case 9: draw(frame0, 7, 0); draw_atlas(2, wx, wy); break;
                default: break;
            }
        }
    };
    const auto draw_guardian_hud = [&]() {
        // as.paint calls p.a(Graphics) only while p.i == 2 (channel loops), so loop-less skills show no HUD.
        if (g.state != 2 || g.id < 0 || g.loops_total <= 0) return;
        // p.a(Graphics) HUD: skill name box at the top with the channel progress bar.
        const std::size_t name_index = std::size_t(g.id * 8 + g.skill * 2);
        if (name_index < guardian_skill_names.size() && dialogue_font_texture) {
            const int x0 = (output_w - 80 * pixel_scale) / 2, s = pixel_scale;
            const auto fill = [&](int r, int gr, int b, int x, int y, int w, int h) {
                SDL_SetRenderDrawColor(renderer, Uint8(r), Uint8(gr), Uint8(b), 255);
                SDL_Rect rect{x0 + x * s, y * s, w * s, h * s};
                SDL_RenderFillRect(renderer, &rect);
            };
            // cb.a(g, x, 2, 80, 25, false): bevel lines 0x3F1F3F outer, 0x9F7F3F light, 0x3F3F3F dark.
            fill(0x3F, 0x1F, 0x3F, 1, 2, 78, 1); fill(0x3F, 0x1F, 0x3F, 79, 3, 1, 23);
            fill(0x3F, 0x1F, 0x3F, 1, 26, 78, 1); fill(0x3F, 0x1F, 0x3F, 0, 3, 1, 23);
            fill(0x9F, 0x7F, 0x3F, 1, 3, 77, 1); fill(0x9F, 0x7F, 0x3F, 1, 3, 1, 22);
            fill(0x3F, 0x3F, 0x3F, 78, 3, 1, 22); fill(0x3F, 0x3F, 0x3F, 1, 25, 78, 1);
            fill(0x5F, 0x1F, 0x3F, 2, 4, 80, 21); // cb text box at translate(x+2, 4), 80x21
            const auto glyphs = layout_bitmap_text(dialogue_font, guardian_skill_names[name_index], 0, 0, 100000, pixel_scale);
            int width = 0;
            for (const auto& glyph : glyphs) width = std::max(width, glyph.x + int(glyph.source_width) * pixel_scale);
            for (const auto& glyph : glyphs) { // centred on x+2+40, one pixel below the box top
                SDL_Rect src{int(glyph.source_x), 0, int(glyph.source_width), int(dialogue_font.height)};
                SDL_Rect dest{x0 + 42 * s - width / 2 + glyph.x, 5 * s + glyph.y,
                              int(glyph.source_width) * pixel_scale, int(dialogue_font.height) * pixel_scale};
                SDL_RenderCopy(renderer, dialogue_font_texture, &src, &dest);
            }
            fill(0, 0, 0, 5, 22, 74, 2);
            fill(0xFF, 0x55, 0x55, 5, 22, 70 * (g.loops_total - g.loops + 1) / g.loops_total, 2);
        }
    };
    // Original aw hit sprites and y death EIFs, separate from guardian effects.
    const auto draw_battle_effect = [&](const combat::BattleEffect& effect) {
        int x=effect.x_px,y=effect.y_px;
        if(effect.target==-1) {x=hero.x_pixels+8;y=hero.y_pixels+8;}
        else if(effect.target>=0 && std::size_t(effect.target)<combat_world.enemies().size()) {
            const auto& target=combat_world.enemies()[effect.target];
            const auto smooth=draw_offset(g_enemy_draw_offsets,std::size_t(effect.target));
            x=target.x_px+smooth.x+8+(target.width-1)*8;y=target.y_px+smooth.y+8+target.draw_offset_y;
        }
        const auto draw_piece=[&](int key,int ox,int oy,bool mirror=false) {
            const auto texture=combat_textures.find(key);if(texture==combat_textures.end()) return;
            int w=0,h=0;SDL_QueryTexture(texture->second,nullptr,nullptr,&w,&h);
            SDL_Rect dest{left+(x+ox)*pixel_scale,top+(y+oy)*pixel_scale,w*pixel_scale,h*pixel_scale};
            SDL_RenderCopyEx(renderer,texture->second,nullptr,&dest,0,nullptr,mirror?SDL_FLIP_HORIZONTAL:SDL_FLIP_NONE);
        };
        if(effect.kind==3) {
            // aw3: ce.s anchored HCENTER|BOTTOM at (x, y+5), hidden every fourth frame.
            if(effect.frame%4<3) {
                const auto texture=combat_textures.find(0x3ff);
                int w=0,h=0;if(texture!=combat_textures.end()) SDL_QueryTexture(texture->second,nullptr,nullptr,&w,&h);
                draw_piece(0x3ff,-w/2,5-h);
            }
            return;
        }
        if(effect.kind==4) {
            // aw4: as.b draws the lvup.eif frame with layers relative to the hero.
            const auto& frames=death_effects[3];
            if(std::size_t(effect.frame)<frames.size()) for(const auto& layer:frames[effect.frame])
                if(layer.atlas_frame!=0xff) draw_piece(0x400+int(layer.atlas_frame),layer.x,layer.y);
            return;
        }
        if(effect.kind>=combat::kEifEffectBase) {
            // aw9 / y: the actor's atef EIF, layers TOP|LEFT from the anchor: aw9 keeps the actor's
            // (px+8, py+8), a world y(BB[B) the tile's (+8, +9).
            const int sprite=effect.kind-combat::kEifEffectBase;
            const auto found=npc_assets.effects.find(sprite);
            if(found==npc_assets.effects.end() || std::size_t(effect.frame)>=found->second.size()) return;
            const int base=sprite>=32?boss_texture_base(sprite):sprite;
            for(const auto& layer:found->second[std::size_t(effect.frame)]) {
                if(layer.atlas_frame==0xff) continue;
                const auto texture=npc_assets.enemy_textures.find((base<<8)|layer.atlas_frame);
                int w=0,h=0;
                if(texture==npc_assets.enemy_textures.end() || SDL_QueryTexture(texture->second,nullptr,nullptr,&w,&h)!=0) continue;
                SDL_Rect dest{left+(x+layer.x)*pixel_scale,top+(y+layer.y)*pixel_scale,w*pixel_scale,h*pixel_scale};
                SDL_RenderCopy(renderer,texture->second,nullptr,&dest);
            }
        } else if(effect.kind>=200) {
            const int icon=effect.kind-200;
            const auto draw_icon=[&](int key,int offset) {
                const auto texture=hero_assets.textures.find(key);if(texture==hero_assets.textures.end()) return;
                int w=0,h=0;SDL_QueryTexture(texture->second,nullptr,nullptr,&w,&h);
                SDL_Rect dest{left+x*pixel_scale-w*pixel_scale/2,top+(y+offset)*pixel_scale,w*pixel_scale,h*pixel_scale};
                SDL_RenderCopy(renderer,texture->second,nullptr,&dest);
            };
            if(icon!=8 && icon!=9) draw_icon(0x20000,-40);
            draw_icon(0x10000+icon,-39+effect.frame%2);
        } else if(effect.kind>=100 && effect.kind<=102) {
            const auto& frames=death_effects[effect.kind-100];
            if(std::size_t(effect.frame)<frames.size()) for(const auto& layer:frames[effect.frame])
                draw_piece(0x200+int(layer.atlas_frame),layer.x,layer.y);
        } else {
            const int bank=effect.kind==6?2:effect.kind==5?1:0;
            const int key=0x100+bank*16+effect.frame;
            const auto texture=combat_textures.find(key);if(texture==combat_textures.end()) return;
            int w=0,h=0;SDL_QueryTexture(texture->second,nullptr,nullptr,&w,&h);
            int dx=0,dy=3;
            if(effect.kind==1) {if(effect.frame==0) {dx=-3;dy-=10;}else if(effect.frame==1)dy-=8;}
            if(effect.kind==5 && effect.frame==2)dy-=5;
            if(effect.kind==6) {if(effect.frame==1)dy-=2;else if(effect.frame==2)dy-=6;}
            draw_piece(key,dx-w/2,dy-h);
        }
    };
    // aw: damage numbers rise 4 px per frame from 30 px above the actor; MISS uses etcui 1.
    const auto draw_popup = [&](const combat::Popup& popup) {
        int px = popup.x_px, py = popup.y_px;
        if (popup.target == -1) { px = hero.x_pixels + 8; py = hero.y_pixels + 8; } // o.b passes the owner's anchor
        else if (popup.target >= 0 && std::size_t(popup.target) < combat_world.enemies().size()) {
            const auto& owner = combat_world.enemies()[std::size_t(popup.target)];
            if (owner.generation == popup.generation && owner.state != combat::EnemyState::dead) {
                const auto smooth = draw_offset(g_enemy_draw_offsets, std::size_t(popup.target));
                px = owner.x_px + smooth.x + 8 + (owner.width - 1) * 8; py = owner.y_px + smooth.y + 8 + owner.draw_offset_y;
            }
        }
        const int x = left + px * pixel_scale, y = top + (py - 30 - popup.age * 4) * pixel_scale;
        if (popup.kind == combat::PopupKind::miss) {
            const auto texture = combat_textures.find(1);
            int w = 0, h = 0;
            if (texture != combat_textures.end() && SDL_QueryTexture(texture->second, nullptr, nullptr, &w, &h) == 0) {
                SDL_Rect dest{x - w * pixel_scale / 2, y, w * pixel_scale, h * pixel_scale};
                SDL_RenderCopy(renderer, texture->second, nullptr, &dest);
            }
            return;
        }
        // aw7 / r.a: two large frames (9x14, advance8), then two small
        // frames (7x9, advance6). Enemy damage uses strips7/5; hero damage8/6.
        const auto number=combat::damage_number_frame(popup.value,popup.age);
        const auto strip=combat_textures.find(number.strip);if(strip==combat_textures.end()) return;
        const int glyph_width=number.width,glyph_height=number.height,advance=number.advance;
        const std::string digits=std::to_string(std::abs(popup.value));
        const int start=x+pixel_scale-int(digits.size())*advance*pixel_scale/2;
        for(std::size_t i=0;i<digits.size();++i) {
            SDL_Rect src{(digits[i]-'0')*glyph_width,0,glyph_width,glyph_height};
            SDL_Rect dest{start+int(i)*advance*pixel_scale,y,glyph_width*pixel_scale,glyph_height*pixel_scale};
            SDL_RenderCopy(renderer,strip->second,&src,&dest);
        }
    };
    // o.c / cf: reverse insertion order, twelve-pixel spacing, TOP|HCENTER.
    const auto draw_status=[&](int id,int elapsed,int x,int y) {
        const auto piece=[&](int key,int offset) {
            const auto texture=combat_textures.find(key);if(texture==combat_textures.end()) return;
            int w=0,h=0;SDL_QueryTexture(texture->second,nullptr,nullptr,&w,&h);
            SDL_Rect dest{left+x*pixel_scale-w*pixel_scale/2,top+(y+offset)*pixel_scale,w*pixel_scale,h*pixel_scale};
            SDL_RenderCopy(renderer,texture->second,nullptr,&dest);
        };
        piece(0x300,-30);piece(0x301+id,-29+elapsed%2);
    };
    const auto draw_enemy_status = [&](std::size_t index) {
        const auto& enemy=combat_world.enemies()[index];
        if(!enemy.on_screen || enemy.state==combat::EnemyState::dead || enemy.hidden || enemy.status_order.empty()) return;
        const auto smooth=draw_offset(g_enemy_draw_offsets,index);
        int x=enemy.x_px+smooth.x+8+(enemy.width-1)*8-6*(int(enemy.status_order.size())-1);
        int size=0;
        if(const auto* stats=combat_world.stats();stats && std::size_t(enemy.stats)<stats->size()) size=(*stats)[enemy.stats].size;
        for(auto it=enemy.status_order.rbegin();it!=enemy.status_order.rend();++it,x+=12)
            draw_status(*it,enemy.status_elapsed[*it],x,enemy.y_px+smooth.y+8+enemy.draw_offset_y-size*3);
    };
    const auto draw_hero_status = [&]() {
        const auto& hero_status=combat_world.hero_status();
        int status_x=hero.x_pixels+8-6*(int(hero_status.order.size())-1);
        for(auto it=hero_status.order.rbegin();it!=hero_status.order.rend();++it,status_x+=12) {
            const int elapsed=*it==5?140-hero_status.attack_up:*it==6?160-hero_status.defense_up:hero_status.poison_tick;
            draw_status(*it,elapsed,status_x,hero.y_pixels);
        }
    };
    // aq: one depth chain (physical Y + anchor offset) for actors, objects and the world-bound
    // effects y/i and the guardian p; attached aw/bj are drawn by their owner (o.b).
    const auto draw_attached = [&](int owner) {
        for (const auto& a : combat_world.auras()) if (a.target == owner) draw_aura(a);
        for (const auto& effect : combat_world.battle_effects()) if (effect.target == owner) draw_battle_effect(effect);
        for (const auto& popup : combat_world.popups()) if (popup.target == owner) draw_popup(popup);
        if (owner == -1) draw_hero_status();
        else if (owner >= 0 && std::size_t(owner) < combat_world.enemies().size()) draw_enemy_status(std::size_t(owner));
    };
    enum class Extra { projectile, world_effect, death_effect, guardian };
    struct ExtraEntry { int depth; Extra kind; std::size_t index; };
    std::vector<ExtraEntry> extras;
    for (std::size_t i = 0; i < combat_world.projectiles().size(); ++i)
        extras.push_back({combat_world.projectiles()[i].y * 16 + 9, Extra::projectile, i});
    if (fx) for (std::size_t i = 0; i < combat_world.effects().size(); ++i)
        extras.push_back({combat_world.effects()[i].y_px + 9, Extra::world_effect, i});
    for (std::size_t i = 0; i < combat_world.battle_effects().size(); ++i)
        if (combat_world.battle_effects()[i].target == -2) extras.push_back({combat_world.battle_effects()[i].y_px, Extra::death_effect, i});
    if (g.state != 0 && g.id >= 0) extras.push_back({g.y * 16 + 8, Extra::guardian, 0});
    std::stable_sort(extras.begin(), extras.end(), [](const auto& a, const auto& b) { return a.depth < b.depth; });
    const auto draw_extra = [&](const ExtraEntry& x) {
        switch (x.kind) {
            case Extra::projectile: draw_projectile(combat_world.projectiles()[x.index]); break;
            case Extra::world_effect: draw_world_effect(combat_world.effects()[x.index]); break;
            case Extra::death_effect: draw_battle_effect(combat_world.battle_effects()[x.index]); break;
            case Extra::guardian: draw_guardian(); break;
        }
    };
    std::size_t next_extra = 0;
    for(const auto& entry:world_draw_order(objects,npcs,enemies,hero.y_pixels)) {
        // Effects sort after actors of the same depth (they are added to aq later).
        while (next_extra < extras.size() && extras[next_extra].depth < entry.depth) draw_extra(extras[next_extra++]);
        switch(entry.kind) {
            case WorldDrawKind::object: draw_object(entry.index); break;
            case WorldDrawKind::npc: draw_npc(entry.index); break;
            case WorldDrawKind::enemy:
                draw_enemies(renderer,{enemies[entry.index]},map,npc_assets,left,top,cell,pixel_scale,
                             &combat_world,&combat_textures);
                draw_attached(enemies[entry.index].frame_index);
                break;
            case WorldDrawKind::hero: draw_hero(); draw_attached(-1); break;
        }
    }
    while (next_extra < extras.size()) draw_extra(extras[next_extra++]);
    // Owners without a sprite in the depth chain (enemies not mirrored into MapEntity), world popups.
    const auto in_chain = [&](int owner) {
        return std::any_of(enemies.begin(), enemies.end(), [&](const MapEntity& m) { return m.frame_index == owner; });
    };
    for (std::size_t i = 0; i < combat_world.enemies().size(); ++i) if (!in_chain(int(i))) draw_attached(int(i));
    for (const auto& popup : combat_world.popups()) if (popup.target < -1) draw_popup(popup);
    draw_guardian_hud();
    const auto draw_emote = [&](std::uint8_t id, int actor_x, int actor_y) {
        if (id == 0) return;
        const int frame = id - 1;
        const int anchor_x = left + (actor_x + 8) * pixel_scale;
        const int anchor_y = top + (actor_y + 8) * pixel_scale;
        const auto draw_top_center = [&](int key, int offset_y) {
            const auto texture = hero_assets.textures.find(key);
            if (texture == hero_assets.textures.end()) return;
            int w = 0, h = 0;
            if (SDL_QueryTexture(texture->second, nullptr, nullptr, &w, &h) != 0) return;
            SDL_Rect dest{anchor_x - w * pixel_scale / 2, anchor_y + offset_y * pixel_scale,
                          w * pixel_scale, h * pixel_scale};
            SDL_RenderCopy(renderer, texture->second, nullptr, &dest);
        };
        // aw type 10: backing except frames 8/9, then icon at -39 with 1px bob.
        if (frame != 8 && frame != 9) draw_top_center(0x20000, -40);
        draw_top_center(0x10000 + frame, -39 + static_cast<int>((SDL_GetTicks() / 80) % 2));
    };
    draw_emote(event_visuals.hero_emote, hero.x_pixels, hero.y_pixels);
    for (std::size_t i = 0; i < npcs.size() && i < event_visuals.npc_emotes.size(); ++i) {
        if (i < event.npc_visible.size() && !event.npc_visible[i]) continue;
        draw_emote(event_visuals.npc_emotes[i], int(npcs[i].x) * 16 + (npcs[i].partial_x ? 8 : 0),
                   int(npcs[i].y) * 16 + (npcs[i].partial_y ? 8 : 0));
    }
    if (event_visuals.world_hidden || event_visuals.flash_white) {
        const Uint8 shade=event_visuals.flash_white?255:0;
        SDL_SetRenderDrawColor(renderer, shade, shade, shade, 255);
        SDL_RenderFillRect(renderer, nullptr);
    }
    }
#if HEROES_LORE_3DS
    phase(0);
#endif
    HL3D_DEPTH(0.0f); // the interface stays on the screen plane
    const std::function<void(SDL_Rect)> hero_preview=[&](SDL_Rect box) {
        HeroState preview;
        preview.direction=2; preview.motion_mode=1;
        const int scale=std::max(1,std::min(box.w/32,box.h/40));
        const int left=box.x+box.w/2-8*scale;
        const int top=box.y+box.h-10*scale;
        draw_hero_sprite(renderer,preview,hero_assets,left,top,scale);
    };
    [[maybe_unused]] const bool menu_screen=ui_model.screen==VisualScreen::game_menu || ui_model.screen==VisualScreen::shop ||
        (ui_model.screen==VisualScreen::service && ui_model.service);
#if HEROES_LORE_3DS
    if(title_below) {
        VisualUiModel top=ui_model;
        if(top.startup.in_game) {top.screen=VisualScreen::none;top.world_hidden=false;}
        else {top.startup.page=TitlePage::logo;top.logo_prompt=false;} // the menu is open: no PRESS ANY KEY
        draw_visual_ui(renderer,top,dialogue_textures,dialogue_font_texture,dialogue_font,output_w,output_h,hero_preview);
    } else
    if(menu_screen && ui_model.game_over_chars<0) { // the menu opens on the bottom screen; the field stays on top
        VisualUiModel field=ui_model;field.screen=VisualScreen::none; // menu prompts and notices stay below
        field.confirming=false;field.notice.clear();field.modal_title.clear();field.modal_text.clear();
        draw_visual_ui(renderer,field,dialogue_textures,dialogue_font_texture,dialogue_font,output_w,output_h,hero_preview);
    } else
#endif
    if(ui_model.game_over_chars<0) draw_visual_ui(renderer,ui_model,dialogue_textures,dialogue_font_texture,dialogue_font,
        output_w,output_h,hero_preview);
#if !HEROES_LORE_CONSOLE
    // Design export: the 3DS bottom screen as separate 320x240 layers (raw BGRA, transparent
    // background) in the folder HEROES_LORE_SNAPSHOT_BOTTOM names; tools turn them into PNGs.
    if(const char* folder=std::getenv("HEROES_LORE_SNAPSHOT_BOTTOM")) {
        SDL_Texture* target=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_TARGET,320,240);
        if(!target) throw std::runtime_error(SDL_GetError());
        SDL_SetTextureBlendMode(target,SDL_BLENDMODE_BLEND);
        VisualUiModel sample=ui_model;
        sample.bottom_pass=true;sample.screen=VisualScreen::none;
        if(sample.enemy_name.empty()) {sample.enemy_name="Enemy";sample.enemy_level=5;sample.enemy_hp=30;sample.enemy_max_hp=50;}
        const auto layer=[&](const std::string& name,unsigned mask,const VisualUiModel& model,bool menu=false) {
            SDL_SetRenderTarget(renderer,target);
            SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer,0,0,0,0);
            SDL_RenderClear(renderer);
            VisualUiModel copy=model;copy.bottom_layers=mask;
            draw_bottom_screen(renderer,copy,dialogue_textures,dialogue_font_texture,dialogue_font,!menu,menu,hero_preview);
            std::vector<Uint8> pixels(320u*240u*4u);
            const bool read=SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_BGRA32,pixels.data(),320*4)==0;
            SDL_SetRenderTarget(renderer,nullptr);
            if(!read) throw std::runtime_error(SDL_GetError());
            if(auto* file=std::fopen((std::string(folder)+"/"+name+".bgra").c_str(),"wb")) {
                std::fwrite(pixels.data(),1,pixels.size(),file);std::fclose(file);
            } else throw std::runtime_error("Unable to write bottom-screen layer "+name);
        };
        layer("0-composite",15,sample);
        layer("1-background",1,sample);
        layer("2-minimap",2,sample);
        layer("4-hud",4,sample);
        layer("5-enemy-panel",8,sample);
        VisualUiModel indoor=sample;indoor.bottom_minimap.reset();
        if(indoor.location_name.empty()) indoor.location_name="Location";
        layer("3-indoor-plate",2,indoor);
        layer("0-composite-indoor",15,indoor);
        if(menu_screen) {VisualUiModel menu=ui_model;menu.bottom_pass=true;layer("6-menu",15,menu,true);} // as the 3DS shows it below
        SDL_DestroyTexture(target);
    }
    if(const char* path=std::getenv("HEROES_LORE_SNAPSHOT_OUT")) {
        SDL_Surface* shot=SDL_CreateRGBSurfaceWithFormat(0,output_w,output_h,32,SDL_PIXELFORMAT_RGBA32);
        if(!shot) throw std::runtime_error(SDL_GetError());
        const int read=SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,shot->pixels,shot->pitch);
        const int saved=read==0?SDL_SaveBMP(shot,path):-1;
        SDL_FreeSurface(shot);
        if(saved!=0) throw std::runtime_error("Unable to save UI snapshot");
    }
#endif
#if HEROES_LORE_3DS
    phase(1);
#endif
    SDL_RenderPresent(renderer);
#if HEROES_LORE_3DS
    present_3ds_frame();
    phase(2);
    {
        const bool hud=ui_model.game_over_chars<0 && !ui_model.world_hidden && !menu_screen &&
            ui_model.screen!=VisualScreen::title_menu && ui_model.screen!=VisualScreen::ending;
        draw_3ds_bottom(renderer,ui_model,dialogue_textures,dialogue_font_texture,dialogue_font,hud,
                        (menu_screen || title_below) && ui_model.game_over_chars<0,hero_preview);
#ifdef HEROES_LORE_3DS_GPU
        hl3d::end_frame();
#endif
        phase(3);
    }
#endif
}
