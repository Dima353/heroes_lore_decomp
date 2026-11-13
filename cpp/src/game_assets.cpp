#include <SDL.h>
#include "gpu_3ds.hpp"
#include "game_assets.hpp"
#include "image_texture.hpp"
#include "startup_log.hpp"
#include <cstdio>
#include <stdexcept>

void destroy_textures(SpriteTextures& textures) {
    for (const auto& pair : textures) SDL_DestroyTexture(pair.second);
    textures.clear();
}

SDL_Texture* create_image_texture(SDL_Renderer* renderer, const DecodedImage& image) {
    auto* texture = SDL_CreateTexture(renderer, kImageTextureFormat,
                                      SDL_TEXTUREACCESS_STATIC,
                                      static_cast<int>(image.width),
                                      static_cast<int>(image.height));
    if (!texture) throw std::runtime_error(std::string("SDL_CreateTexture: ") + SDL_GetError());
    if (upload_image_texture(texture, image.rgba.data(), static_cast<int>(image.width),
                          static_cast<int>(image.height)) != 0) {
        SDL_DestroyTexture(texture);
        throw std::runtime_error(std::string("SDL_UpdateTexture: ") + SDL_GetError());
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_ScaleModeNearest);
    return texture;
}

SpriteTextures load_sprite_textures(SDL_Renderer* renderer, const GameArchive& archive,
                                           const std::string& stem,
                                           const std::unordered_set<std::size_t>& frame_ids,
                                           const std::vector<std::pair<std::uint32_t,std::uint32_t>>& recolors) {
    SpriteTextures textures;
    if (frame_ids.empty()) return textures;
    try {
        const std::vector<std::size_t> requested(frame_ids.begin(), frame_ids.end());
        const auto frames = load_sprite_frames_png(archive, stem, requested);
        for (const auto& frame : frames) {
            auto image = decode_indexed_png(frame.second);
            // br.a(int,int): palette colour replacements, applied to the decoded pixels.
            for (std::size_t p = 0; !recolors.empty() && p + 3 < image.rgba.size(); p += 4) {
                if (!image.rgba[p + 3]) continue;
                const auto rgb = std::uint32_t(image.rgba[p]) << 16 | std::uint32_t(image.rgba[p + 1]) << 8 | image.rgba[p + 2];
                for (const auto& recolor : recolors) if (rgb == recolor.first) {
                    image.rgba[p] = std::uint8_t(recolor.second >> 16);
                    image.rgba[p + 1] = std::uint8_t(recolor.second >> 8);
                    image.rgba[p + 2] = std::uint8_t(recolor.second);
                    break;
                }
            }
            auto* texture = SDL_CreateTexture(renderer, kImageTextureFormat,
                                               SDL_TEXTUREACCESS_STATIC,
                                               static_cast<int>(image.width),
                                               static_cast<int>(image.height));
            if (!texture) throw std::runtime_error(std::string("SDL_CreateTexture: ") + SDL_GetError());
            if (upload_image_texture(texture, image.rgba.data(), static_cast<int>(image.width),
                                  static_cast<int>(image.height)) != 0) {
                SDL_DestroyTexture(texture);
                throw std::runtime_error(std::string("SDL_UpdateTexture: ") + SDL_GetError());
            }
            SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(texture, SDL_ScaleModeNearest);
            textures.emplace(static_cast<int>(frame.first), texture);
        }
    } catch (...) {
        destroy_textures(textures);
        throw;
    }
    return textures;
}

void ensure_sprite_texture(SDL_Renderer* renderer, const GameArchive& archive,
                                  const std::string& stem, int frame_id,
                                  SpriteTextures& cache) {
    if (frame_id < 0 || cache.count(frame_id)) return;
    const auto png = load_sprite_frame_png(archive, stem,
                                           static_cast<std::size_t>(frame_id));
    const auto image = decode_indexed_png(png);
    cache.emplace(frame_id, create_image_texture(renderer, image));
}

SpriteTextures load_terrain_textures(SDL_Renderer* renderer, const GameArchive& archive,
                                            const GameMap& map) {
    std::unordered_set<std::size_t> frame_ids;
    for (const auto frame : map.terrain) frame_ids.insert(frame);
    char stem[16];
    std::snprintf(stem, sizeof(stem), "m/t/t%02u", unsigned(map.kind));
    return load_sprite_textures(renderer, archive, stem, frame_ids);
}

SpriteTextures load_object_textures(SDL_Renderer* renderer, const GameArchive& archive,
                                           const GameMap& map, const EventGrid& event,
                                           const std::vector<MapEntity>& objects) {
    std::unordered_set<std::size_t> frame_ids;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        if (i < event.object_visible.size() && !event.object_visible[i]) continue;
        if (objects[i].frame_index >= 0) frame_ids.insert(static_cast<std::size_t>(objects[i].frame_index));
    }
    char stem[16];
    std::snprintf(stem, sizeof(stem), "m/t/o%02u", unsigned(map.kind));
    return load_sprite_textures(renderer, archive, stem, frame_ids);
}

NpcVisualAssets load_npc_textures(SDL_Renderer* renderer, const GameArchive& archive,
                                         const std::vector<MapEntity>& npcs) {
    NpcVisualAssets assets;
    std::unordered_set<std::size_t> static_frames;
    for (const auto& npc : npcs) {
        if (npc.frame_index >= 0) {
            static_frames.insert(static_cast<std::size_t>(npc.frame_index));
            continue;
        }
        const int code = npc.code >= 128 ? int(npc.code) - 256 : npc.code;
        if (code < 0 || code >= 18 || assets.animations.count(code)) continue;
        if (code == 3 || code == 6) {
            assets.animations.emplace(code,
                load_enemy_animations(archive, code == 3 ? 17 : 20));
        } else {
            assets.animations.emplace(code, load_npc_animations(archive, code));
        }
    }
    auto static_textures = load_sprite_textures(renderer, archive, "npc/all", static_frames);
    for (const auto& texture : static_textures)
        assets.textures.emplace(0x10000 + texture.first, texture.second);
    for (const auto& animation : assets.animations) {
        std::unordered_set<std::size_t> frames;
        const auto collect_frames = [&](const auto& motion) {
            for (const auto& direction : motion)
                for (const auto& frame : direction)
                    for (const auto& layer : frame) frames.insert(layer.frame);
        };
        collect_frames(animation.second.idle);
        collect_frames(animation.second.walk);
        char stem[16];
        const char* atlas_family = animation.first == 3 || animation.first == 6 ? "enm" : "npc";
        const int atlas_id = animation.first == 3 ? 17
                           : animation.first == 6 ? 20 : animation.first;
        std::snprintf(stem, sizeof(stem), "%s/%02d", atlas_family, atlas_id);
        auto atlas_textures = load_sprite_textures(renderer, archive, stem, frames);
        for (const auto& texture : atlas_textures)
            assets.textures.emplace((animation.first << 8) | texture.first, texture.second);
    }
    return assets;
}

void load_enemy_textures(SDL_Renderer* renderer, const GameArchive& archive,
                                const std::vector<MapEntity>& enemies, NpcVisualAssets& assets) {
    for (const auto& enemy : enemies) {
        if (enemy.resource_slot < 0) continue;
        const int id = enemy.code >= 128 ? int(enemy.code) - 256 : enemy.code;
        if (id < 0 || id > 31 || assets.enemy_idle_frames.count(id)) continue;
        assets.enemy_idle_frames.emplace(id, load_enemy_idle_frame(archive, id));
        assets.enemy_animations.emplace(id, load_enemy_animations(archive, id));
        char effect[24];
        std::snprintf(effect, sizeof(effect), "enm/atef/%02d", id);
        if (archive.contains(effect)) assets.effects.emplace(id, load_eif(archive, effect));
    }
    for (const auto& animation : assets.enemy_idle_frames) {
        std::unordered_set<std::size_t> frames;
        for (const auto& layer : animation.second) frames.insert(layer.frame);
        const auto found = assets.enemy_animations.find(animation.first);
        if (found != assets.enemy_animations.end())
            for (const auto* set : {&found->second.idle, &found->second.walk, &found->second.attack})
                for (const auto& direction : *set)
                    for (const auto& frame : direction)
                        for (const auto& layer : frame) frames.insert(layer.frame);
        const auto effect = assets.effects.find(animation.first);
        if (effect != assets.effects.end())
            for (const auto& frame : effect->second)
                for (const auto& layer : frame) frames.insert(layer.atlas_frame);
        char stem[16];
        std::snprintf(stem, sizeof(stem), "enm/%02d", animation.first);
        auto textures = load_sprite_textures(renderer, archive, stem, frames);
        for (const auto& texture : textures)
            assets.enemy_textures.emplace((animation.first << 8) | texture.first, texture.second);
    }
}

HeroVisualAssets load_hero_textures(SDL_Renderer* renderer,
                                           const GameArchive& archive,
                                           int character_class, const EquipmentSet& equipment, int element) {
    HeroVisualAssets assets;
    assets.element = element;
    // bu: the weapon atlas' blue (0x0000ff) glow takes the guardian's element colour.
    static constexpr std::uint32_t glow[4]{0x0000ff, 0xff7f3f, 0x5f7fbf, 0x7fff7f};
    const auto layers = equipment_visual_layers(character_class, equipment);
    try {
        if (character_class == 8)
            for (int i = 0; i < 3; ++i) assets.magic[std::size_t(i)] = load_eif(archive, "c3/s/ea" + std::to_string(i + 2));
        for (std::size_t category = 0; category < layers.size(); ++category) {
            if (!layers[category].enabled) continue;
            startup_log("loading hero sprite category: " + layers[category].category + "/" + layers[category].atlas);
            assets.tables[category] = load_character_sprite_table(
                archive, character_class, layers[category].category);
            char stem[24];
            std::snprintf(stem, sizeof(stem), "c%d/i/%s", character_class - 5,
                          layers[category].atlas.c_str());
            assets.mirror_on_load[category] = load_sprite_atlas(archive, stem).mirror_on_load();

            std::unordered_set<std::size_t> frame_ids;
            for (const auto& record : assets.tables[category]) {
                for (const auto& frame : record.frames)
                    for (const auto& layer : frame) {
                        // bu stores -1 as 0xff for an absent sprite layer.
                        // Its renderer skips this sentinel before touching the atlas.
                        if (layer.atlas_frame != 0xff)
                            frame_ids.insert(layer.atlas_frame);
                    }
            }
            if (category == 5)
                for (const auto& animation : assets.magic)
                    for (const auto& frame : animation)
                        for (const auto& layer : frame) frame_ids.insert(layer.atlas_frame);
            // bu: weapon glow 0x0000ff and the swing trails' three blues follow the guardian element.
            std::vector<std::pair<std::uint32_t,std::uint32_t>> recolors;
            if (category == 3 && element >= 1 && element <= 3) recolors.push_back({0x0000ffu, glow[element]});
            if (category == 5 && element == 1) recolors = {{0xbfdfff,0xffffc0},{0x9fbfff,0xffbf7f},{0x5f7fbf,0xff7f3f}};
            if (category == 5 && element == 3) recolors = {{0xbfdfff,0xdfffbf},{0x9fbfff,0xbfdfbf},{0x5f7fbf,0x9fbf7f}};
            auto textures = load_sprite_textures(renderer, archive, stem, frame_ids, recolors);
            for (const auto& texture : textures)
                assets.textures.emplace((static_cast<int>(category) << 8) | texture.first,
                                        texture.second);
        }
        const auto emotes = load_sprite_atlas(archive, "img/emoti");
        std::unordered_set<std::size_t> emote_frames;
        for (std::size_t i = 0; i < emotes.frames.size(); ++i) emote_frames.insert(i);
        auto emote_textures = load_sprite_textures(renderer, archive, "img/emoti", emote_frames);
        for (const auto& entry : emote_textures)
            assets.textures.emplace(0x10000 + entry.first, entry.second);
        auto backing = load_sprite_textures(renderer, archive, "img/keepst", {0});
        assets.textures.emplace(0x20000, backing.at(0));
    } catch (...) {
        destroy_textures(assets.textures);
        throw;
    }
    return assets;
}
