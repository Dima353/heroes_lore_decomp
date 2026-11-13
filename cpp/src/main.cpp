#include "texture_tiles.hpp"
#include "frame_timing.hpp"
#include <SDL.h>
#include "bitmap_font.hpp"
#include "combat.hpp"
#include "character_sprite.hpp"
#include "event_runtime.hpp"
#include "event_trigger.hpp"
#include "new_game.hpp"
#include "equipment.hpp"
#include "viewport.hpp"
#include "visual_ui.hpp"
#include "image_texture.hpp"
#include "world_presentation.hpp"
#include "consumable.hpp"
#include "save_game.hpp"
#include "title_menu.hpp"
#include "port_settings.hpp"
#include "dialogue_reveal.hpp"
#include "event_visual.hpp"
#include "hero_movement.hpp"
#include "language.hpp"
#include "item_catalog.hpp"
#include "inventory_rules.hpp"
#include "shop_transaction.hpp"
#include "service.hpp"
#include "ending.hpp"
#include "audio.hpp"
#include "map.hpp"
#include "language_pack.hpp"
#include "platform.hpp"
#include "embedded_soundfont.hpp"
#include "gpu_3ds.hpp"
#if HEROES_LORE_SWITCH
#include "switch_rumble.hpp"
#endif
#if HEROES_LORE_VITA
#include <psp2/power.h>
#endif
#if HEROES_LORE_PSP
#include <psppower.h>
#include "platform_psp.hpp"
#include "psp_diagnostics.hpp"
#endif
#if HEROES_LORE_3DS
#include <3ds.h>
// SDL's N3DS window surface is RGBA8888, a format without fast blitters in SDL 2 (every sprite went
// through SDL_Blit_Slow). The game draws into an ARGB8888 surface (see image_texture.hpp) and
// this rotates it into the top screen's framebuffer (240x400, column-major, RGBA8 packed).
static SDL_Surface* g_3ds_frame=nullptr;
// libctru's default split caps the normal heap at 24 MB and gives the rest (~25 MB on Old 3DS) to the
// linear heap, which only the framebuffers and DSP buffers use (~2 MB here). The embedded SoundFont
// alone needs ~13 MB of normal heap, so keep 10 MB linear and the rest (~40 MB) for malloc.
#ifdef HEROES_LORE_3DS_GPU
extern "C" { u32 __ctru_linear_heap_size = 16u*1024u*1024u; } // + GPU textures and citro2d vertices
#else
extern "C" { u32 __ctru_linear_heap_size = 10u*1024u*1024u; }
#endif
static bool g_3ds_audio_core=false; // SDL's audio thread got 80% of the system core
constexpr Uint8 kTouchTap=0x7A; // padding1 of controller events made from bottom-screen taps
// HOME menu and sleep: SDL's audio thread runs on the system core, which the HOME menu needs.
// While suspended the audio pauses and the core limit drops (as the sm64 3DS port does).
static SDL_AudioDeviceID g_3ds_audio=0;
static aptHookCookie g_3ds_apt_cookie;
static void apt_hook_3ds(APT_HookType hook,void*) {
    if(!g_3ds_audio) return;
    if(hook==APTHOOK_ONSUSPEND || hook==APTHOOK_ONSLEEP || hook==APTHOOK_ONEXIT) {
        SDL_PauseAudioDevice(g_3ds_audio,1);
        APT_SetAppCpuTimeLimit(30);
    } else if(hook==APTHOOK_ONRESTORE || hook==APTHOOK_ONWAKEUP) {
        APT_SetAppCpuTimeLimit(80);
        SDL_PauseAudioDevice(g_3ds_audio,0);
    }
}
static Uint32 g_3ds_bottom_redraws=0; // full bottom-screen redraws (perf line)
static Uint32 g_3ds_bottom_ms[3]{};    // of those: draw, read-back, compare + present
static Uint32 g_3ds_input_serial=0;    // bumped by every button, key or touch event
#ifndef HEROES_LORE_3DS_GPU
static void present_3ds_screen(gfxScreen_t screen,const void* pixels,int w,int h,int pitch) {
    u16 fb_w=0,fb_h=0;
    auto* dst=reinterpret_cast<u32*>(gfxGetFramebuffer(screen,GFX_LEFT,&fb_w,&fb_h));
    w=std::min<int>(w,fb_h);h=std::min<int>(h,fb_w);
    // Bands of 8 source rows: each column then writes 8 neighbouring words (one cache line)
    // instead of one word every 960 bytes.
    for(int y0=0;y0<h;y0+=8) {
        const int rows=std::min(8,h-y0);
        const u32* src[8];
        for(int i=0;i<rows;++i) src[i]=reinterpret_cast<const u32*>(static_cast<const u8*>(pixels)+(y0+i)*pitch);
        for(int x=0;x<w;++x) {
            u32* out=dst+x*fb_w+(fb_w-1-y0);
            for(int i=0;i<rows;++i) out[-i]=(src[i][x]<<8)|0xFFu; // ARGB8888 -> RGBA8888
        }
    }
    // A kernel cache flush, not GSP's: GSP runs on the system core, which the audio thread keeps busy.
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE,u32(reinterpret_cast<std::uintptr_t>(dst)),u32(fb_w)*fb_h*4);
    gfxScreenSwapBuffers(screen,false);
}
// Part of a single-buffered screen (the bottom one): rotate just that rectangle into place.
static void present_3ds_region(gfxScreen_t screen,const void* pixels,int pitch,SDL_Rect r) {
    u16 fb_w=0,fb_h=0;
    auto* dst=reinterpret_cast<u32*>(gfxGetFramebuffer(screen,GFX_LEFT,&fb_w,&fb_h));
    r.w=std::min<int>(r.x+r.w,fb_h)-r.x;r.h=std::min<int>(r.y+r.h,fb_w)-r.y;
    if(r.x<0 || r.y<0 || r.w<=0 || r.h<=0) return;
    for(int y=r.y;y<r.y+r.h;++y) {
        const auto* src=reinterpret_cast<const u32*>(static_cast<const u8*>(pixels)+y*pitch);
        u32* out=dst+(fb_w-1-y);
        for(int x=r.x;x<r.x+r.w;++x) out[x*fb_w]=(src[x]<<8)|0xFFu;
    }
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE,u32(reinterpret_cast<std::uintptr_t>(dst)),u32(fb_w)*fb_h*4);
}
static void present_3ds_frame() {
    if(g_3ds_frame) present_3ds_screen(GFX_TOP,g_3ds_frame->pixels,g_3ds_frame->w,g_3ds_frame->h,g_3ds_frame->pitch);
}
// Bottom screen (320x240): the HUD strip, drawn through a render target of the same software renderer
// (SDL textures belong to one renderer) and read back every frame.
static void draw_3ds_bottom(SDL_Renderer* renderer,const VisualUiModel& ui,const UiTextureMap& textures,
                            SDL_Texture* font_texture,const BitmapFont& font,bool hud,bool menu,
                            const std::function<void(SDL_Rect)>& hero_preview) {
    static SDL_Texture* target=nullptr;
    static std::vector<Uint32> pixels(320u*240u),base; // shown picture; the same without the hero's cell
    // The field layout in four bands (draw_bottom_screen): enemy bar, minimap, HP/SP row, buttons.
    // Each has a signature of what it shows; only changed bands are redrawn (clipped), read back
    // and sent, so a hit that changes HP redraws a 30 px strip, not the whole screen (~70 ms on Old 3DS).
    static constexpr SDL_Rect bands[4]{{0,0,320,30},{0,30,320,120},{0,150,320,30},{0,180,320,60}};
    std::string signature[4];
    const auto add=[](std::string& to,long long value){to+=',';to+=std::to_string(value);};
    const std::string mode=menu?"menu":hud?"hud":
        ui.screen==VisualScreen::title_menu && ui.startup.page==TitlePage::logo?"logo":"off"; // logo: the touch prompt
    if(hud && !menu) {
        auto& enemy=signature[0];
        enemy=ui.enemy_name;add(enemy,ui.enemy_level);add(enemy,ui.enemy_hp);add(enemy,ui.enemy_max_hp);add(enemy,ui.enemy_rank);
        auto& map=signature[1];
        map=ui.location_name;
        if(ui.screen==VisualScreen::dialogue || ui.screen==VisualScreen::choice || ui.screen==VisualScreen::notice) {
            map+='|';map+=ui.text; // the dialogue window replaces the minimap
            add(map,int(ui.screen));add(map,ui.item_box);add(map,ui.portrait);add(map,ui.scroll_lines);
            add(map,static_cast<long long>(std::min<std::size_t>(ui.dialogue_visible_end,1u<<30)));
        }
        if(ui.bottom_minimap) {
            add(map,ui.bottom_minimap->width);add(map,ui.bottom_minimap->height); // the hero's cell is patched in place
            std::uint32_t hash=2166136261u;
            for(const auto cell:ui.bottom_minimap->cells) hash=(hash^cell)*16777619u;
            add(map,hash);
        }
        auto& stats=signature[2];
        add(stats,ui.vitals.hp);add(stats,ui.vitals.max_hp);add(stats,ui.vitals.sp);add(stats,ui.vitals.max_sp);
        add(stats,ui.progress.experience);add(stats,ui.progress.next_experience());
        auto& buttons=signature[3];
        add(buttons,ui.progress.attribute_points>0);add(buttons,ui.quick_item_type);add(buttons,ui.quick_item_count);
        add(buttons,static_cast<long long>(ui.guardians.equipped));
        if(ui.guardians.equipped<ui.guardians.slots.size() && ui.guardians.slots[ui.guardians.equipped]) {
            const auto& active=*ui.guardians.slots[ui.guardians.equipped];
            add(buttons,active.id);add(buttons,active.calm);add(buttons,active.wild);
        }
        for(int i=0;i<2;++i) { // the cooldown shade's height (30 px icon), not every tick of the counter
            const int left=ui.guardian_cooldown[std::size_t(i)],full=ui.guardian_cooldown_max[std::size_t(i)];
            add(buttons,left>0 && full>0?30*left/full+1:0);
        }
        buttons+=ui.key_guardian_calm+ui.key_guardian_wild+ui.key_item_use+ui.key_item_next;
        add(buttons,int(ui.screen));add(buttons,ui.item_box);add(buttons,ui.choice_yes); // answer buttons over the row
    }
    static std::string last_mode,last[4];
    static int hero_x=-1,hero_y=-1;
    const bool talking=ui.screen==VisualScreen::dialogue || ui.screen==VisualScreen::choice || ui.screen==VisualScreen::notice;
    const bool minimap=hud && !menu && ui.bottom_minimap && !talking; // the dialogue window covers the minimap
    const int new_x=minimap?ui.bottom_minimap->hero_x:-1,new_y=minimap?ui.bottom_minimap->hero_y:-1;
    const auto paint_cell=[&](int x,int y,bool hero,SDL_Rect* cell) {
        if(!minimap || base.size()!=pixels.size() || !bottom_minimap_cell(*ui.bottom_minimap,x,y,cell)) return false;
        for(int yy=cell->y;yy<cell->y+cell->h;++yy) for(int xx=cell->x;xx<cell->x+cell->w;++xx) {
            const std::size_t i=std::size_t(yy)*320u+std::size_t(xx);
            pixels[i]=hero?0xFFFF3F3Fu:base[i];
        }
        return true;
    };
    // Menus change on input: redraw on it and the two frames after (state that lands a frame late),
    // otherwise twice a second (timed notices); a full menu redraw costs ~75 ms on Old 3DS.
    static Uint32 menu_serial=0,menu_drawn=0;static int menu_after=0;
    if(menu && mode==last_mode && base.size()==pixels.size()) {
        if(g_3ds_input_serial!=menu_serial) {menu_serial=g_3ds_input_serial;menu_after=2;}
        else if(menu_after>0) --menu_after;
        else if(SDL_GetTicks()-menu_drawn<500) return;
    }
    if(menu) menu_drawn=SDL_GetTicks();
    bool redraw[4]{};
    const bool full=menu || mode!=last_mode || base.size()!=pixels.size();
    for(int i=0;i<4;++i) redraw[i]=full || signature[i]!=last[i];
    if(!redraw[0] && !redraw[1] && !redraw[2] && !redraw[3]) { // walking only moves the hero's cell
        if(new_x!=hero_x || new_y!=hero_y) {
            SDL_Rect cell{};
            if(paint_cell(hero_x,hero_y,false,&cell)) present_3ds_region(GFX_BOTTOM,pixels.data(),320*4,cell);
            if(paint_cell(new_x,new_y,true,&cell)) present_3ds_region(GFX_BOTTOM,pixels.data(),320*4,cell);
            hero_x=new_x;hero_y=new_y;
        }
        return;
    }
    last_mode=mode;
    for(int i=0;i<4;++i) last[i]=signature[i];
    if(!target) target=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_TARGET,320,240);
    if(!target || SDL_SetRenderTarget(renderer,target)!=0) return;
    Uint32 mark=SDL_GetTicks();
    const auto step=[&](int index){const Uint32 now=SDL_GetTicks();g_3ds_bottom_ms[index]+=now-mark;mark=now;};
    // One clipped draw over the changed bands (adjacent ones merge into one rectangle).
    SDL_Rect area{0,0,320,240};
    if(!full) {
        int top=240,bottom=0;
        for(int i=0;i<4;++i) if(redraw[i]) {top=std::min(top,bands[i].y);bottom=std::max(bottom,bands[i].y+bands[i].h);}
        area={0,top,320,bottom-top};
    }
    SDL_RenderSetClipRect(renderer,full?nullptr:&area);
    SDL_SetRenderDrawColor(renderer,0,0,0,255);
    SDL_RenderClear(renderer);
    VisualUiModel bottom=ui;bottom.bottom_pass=true;
    if(minimap) {bottom.bottom_minimap->hero_x=-1;bottom.bottom_minimap->hero_y=-1;} // painted separately
    draw_bottom_screen(renderer,bottom,textures,font_texture,font,hud,menu,hero_preview);
    SDL_RenderFlush(renderer);step(0);
    if(base.size()!=pixels.size()) base.assign(pixels.size(),0);
    const bool read=SDL_RenderReadPixels(renderer,&area,SDL_PIXELFORMAT_ARGB8888,base.data()+std::size_t(area.y)*320u,320*4)==0;
    SDL_RenderSetClipRect(renderer,nullptr);
    SDL_SetRenderTarget(renderer,nullptr);step(1);
    if(!read) return;
    ++g_3ds_bottom_redraws;
    std::copy(base.begin()+std::ptrdiff_t(area.y)*320,base.begin()+std::ptrdiff_t(area.y+area.h)*320,pixels.begin()+std::ptrdiff_t(area.y)*320);
    SDL_Rect cell{};
    // A step in the same frame as another band's redraw: the old cell lies outside the redrawn area
    // (minimap band unchanged) and kept its red until now; restore it from the picture underneath.
    if((hero_x!=new_x || hero_y!=new_y) && paint_cell(hero_x,hero_y,false,&cell)) present_3ds_region(GFX_BOTTOM,pixels.data(),320*4,cell);
    hero_x=new_x;hero_y=new_y;
    if(paint_cell(hero_x,hero_y,true,&cell)) present_3ds_region(GFX_BOTTOM,pixels.data(),320*4,cell);
    present_3ds_region(GFX_BOTTOM,pixels.data(),320*4,area); // single-buffered (see renderer setup)
    step(2);
}
#else
// GPU path (gpu_3ds.cpp): both screens are drawn by citro2d; hl3d::end_frame() presents them.
static void present_3ds_frame() {}
static void draw_3ds_bottom(SDL_Renderer* renderer,const VisualUiModel& ui,const UiTextureMap& textures,
                            SDL_Texture* font_texture,const BitmapFont& font,bool hud,bool menu,
                            const std::function<void(SDL_Rect)>& hero_preview) {
    hl3d::target_bottom(true);
    SDL_RenderSetClipRect(renderer,nullptr);
    SDL_SetRenderDrawColor(renderer,0,0,0,255);
    SDL_RenderClear(renderer);
    VisualUiModel bottom=ui;bottom.bottom_pass=true;
    HL3D_DEPTH(0.0f);
    draw_bottom_screen(renderer,bottom,textures,font_texture,font,hud,menu,hero_preview);
    SDL_RenderSetClipRect(renderer,nullptr);
    hl3d::target_bottom(false);
    ++g_3ds_bottom_redraws;
}
#endif
static Uint32 g_3ds_phase_ms[4]{}; // field, interface, top screen copy, bottom screen
#endif
#include "recipe_journal.hpp"
#include "npc_movement.hpp"
#include "npc_sprite.hpp"
#include "eif.hpp"
#include "sprite.hpp"

#include <atomic>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#if !HEROES_LORE_CONSOLE
#include <iostream>
#endif
#include <memory>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>

static std::string startup_log_path;

// Port smoothing: logic stays on its ticks; between two ticks the renderer draws NPCs and
// enemies this many logical pixels away from their current tick position (towards the last one).
static std::vector<SDL_Point> g_npc_draw_offsets, g_enemy_draw_offsets;
// as.paint pairs one logic step with one ac.draw; extra smooth renders must not advance NPC frames.
static std::uint32_t g_logic_tick = 0;
static SDL_Point draw_offset(const std::vector<SDL_Point>& offsets, std::size_t index) {
    return index < offsets.size() ? offsets[index] : SDL_Point{0, 0};
}
// bw: sound ids 0..31 -> /snd files ("def.mid" fills unused ids).
static const char* const kSoundFiles[32]={"00.mid","01.mid","02.mid","03.mid","04.mid","05.mid","06.mid","07.mid",
    "08.wav","def.mid","def.mid","def.mid","12.mid","13.wav","14.wav","15.wav","16.wav","17.wav","18.wav","def.mid",
    "20.wav","21.wav","22.mid","23.mid","24.mid","25.mid","26.mid","27.mid","28.mid","29.mid","30.mid","31.mid"};
// ae.h: field music per map tileset (-1 silent); maps 79..81 play 4.
// ae.a: minimap cell colours by map kind, [kind*2] for open cells and [kind*2+1] for blocked ones.
static constexpr std::uint32_t kMinimapColours[30]={
    0xFFDF3F,0x3F1F3F,0xFFDF3F,0x3F1F3F,0xFFDFBF,0x7F3F3F,0xBFFF3F,0x1F3F3F,0x000000,0x000000,
    0xBFFF3F,0x1F3F3F,0x3FBFFF,0x1F3F3F,0xBFFF3F,0x1F3F3F,0xBFFF3F,0x1F3F3F,0xBFFF3F,0x1F3F3F,
    0x000000,0x000000,0xFFDFBF,0x7F3F3F,0xDFFFFF,0x1F3F7F,0xDFFFFF,0x1F3F7F,0xFFDFBF,0x7F3F3F};
static constexpr int kMapMusic[20]={28,0,27,29,-1,1,26,31,31,2,-1,25,24,24,30,3,-1,-1,-1,-1};
static std::atomic<Uint32> audio_peak_ms{0};
static constexpr const char* kEmbeddedSoundFont="<built-in SoundFont>";
static void audio_callback(void* user,Uint8* stream,int length) {
#if HEROES_LORE_PSP
    psp_configure_fpu();
    psp_diag_audio_stage(1);
#endif
    const Uint32 started=SDL_GetTicks();
    const int frames=length/4; // S16 stereo
    // Plain static, not thread_local: a TLS destructor at SDL's audio-thread exit crashed libnx on quit.
    static std::vector<float> mix;
    const int synth_frames=HEROES_LORE_LOW_POWER?(frames+1)/2:frames;
    mix.resize(std::size_t(synth_frames)*2);
#if HEROES_LORE_PSP
    psp_diag_audio_stage(2);
#endif
    static_cast<AudioMixer*>(user)->render(mix.data(),synth_frames);
#if HEROES_LORE_PSP
    psp_diag_audio_stage(3);
#endif
    auto* out=reinterpret_cast<Sint16*>(stream);
    for(int i=0;i<frames*2;++i) out[i]=Sint16(mix[HEROES_LORE_LOW_POWER?std::size_t((i/4)*2+i%2):std::size_t(i)]*32767.0f);
    if(HEROES_LORE_PSP) {
#if HEROES_LORE_PSP
        psp_diag_audio_stage(0);
#endif
        const Uint32 elapsed=SDL_GetTicks()-started;
        Uint32 previous=audio_peak_ms.load();
        while(elapsed>previous && !audio_peak_ms.compare_exchange_weak(previous,elapsed)) {}
    }
}
// Stops the device before the mixer it renders from goes away (also on exceptions).
// On Switch a thread that exits mid-process crashes in libnx (threadExit -> eh_globals_dtor),
// so SDL's audio thread is only paused there and dies with the process.
struct AudioDevice {
    SDL_AudioDeviceID id{};
    void stop() {
        if(!id) return;
#if HEROES_LORE_3DS
        // Close it: SDL then calls ndspExit. Left running, libctru's DSP thread outlived the heap at exit.
        aptUnhook(&g_3ds_apt_cookie);g_3ds_audio=0;
        SDL_CloseAudioDevice(id);
#elif HEROES_LORE_CONSOLE
        SDL_PauseAudioDevice(id,1); // returns once no callback is running
#else
        SDL_CloseAudioDevice(id);
#endif
        id=0;
    }
    ~AudioDevice() {stop();}
};

static void startup_log(const std::string& message) {
#if !HEROES_LORE_CONSOLE
    std::cerr << "heroes_lore: " << message << '\n';
#endif
    if (startup_log_path.empty()) return;
#if HEROES_LORE_3DS
    // Opening a file on the 3DS SD card costs ~85 ms: keep the log open, flush every line.
    static std::FILE* open_log = nullptr;
    if (!open_log) open_log = std::fopen(startup_log_path.c_str(), "a");
    if (open_log) {
        std::fprintf(open_log, "[%u] %s\n", unsigned(SDL_GetTicks()), message.c_str());
        std::fflush(open_log);
    }
    return;
#endif
    if (auto* log = std::fopen(startup_log_path.c_str(), "a")) {
        std::fputs(message.c_str(), log);
        std::fputc('\n', log);
        std::fflush(log);
        std::fclose(log);
    }
}

[[maybe_unused]] static void reset_startup_log() {
    if (startup_log_path.empty()) return;
    if (auto* log = std::fopen(startup_log_path.c_str(), "w")) std::fclose(log);
}

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
static int guardian_fx_key(int atlas, int frame) { return 0x40000 + atlas * 256 + frame; }

struct HeroVisualAssets {
    SpriteTextures textures;
    std::array<CharacterSpriteTable, 6> tables;
    std::array<bool, 6> mirror_on_load{};
    int element{};   // bu.a(...,p5): guardian element the weapon glow was tinted for
    std::array<EifAnimation, 3> magic{}; // bu.a(br,4,10): Kail's c3/s/ea2..ea4 drawn from the e atlas
    bool peaceful{}; // !ae.c: map kinds 1/5/9/15 hide frame layers 6 and 7 (weapon)
};

static void destroy_textures(SpriteTextures& textures) {
    for (const auto& pair : textures) SDL_DestroyTexture(pair.second);
    textures.clear();
}

static SDL_Texture* create_image_texture(SDL_Renderer* renderer, const DecodedImage& image) {
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

// The window dialogue pages are counted in: 3DS shows it on the bottom screen.
static DialogueLayout shown_dialogue_layout(int output_w, int output_h, int font_height, int portrait) {
#if HEROES_LORE_3DS
    (void)output_w;(void)output_h;
    return calculate_bottom_dialogue_layout(font_height,portrait);
#else
    return calculate_dialogue_layout(output_w,output_h,font_height,portrait);
#endif
}

static int dialogue_line_count(const BitmapFont& font, const std::string& text,
                               int output_w, int output_h, int portrait = 0) {
    const auto layout = shown_dialogue_layout(output_w,output_h,font.height,portrait);
    const int line_height = static_cast<int>(font.height)*layout.scale;
    const auto glyphs = layout_bitmap_text(font,text,0,0,layout.inner.w,layout.scale);
    int last_line = 0;
    for (const auto& glyph : glyphs) last_line = std::max(last_line,glyph.y/line_height);
    return last_line+1;
}

static int direction_from_key(SDL_Keycode key) {
    switch (key) {
        case SDLK_UP: return 1;
        case SDLK_DOWN: return 2;
        case SDLK_LEFT: return 3;
        case SDLK_RIGHT: return 4;
        default: return 0;
    }
}

// SDL uses positional Xbox names on this Switch backend. Translate once so
// all UI actions below use the labels printed on Nintendo controllers.
static Uint8 labelled_controller_button(Uint8 button) {
#if HEROES_LORE_SWITCH
    switch (button) {
        case SDL_CONTROLLER_BUTTON_A: return SDL_CONTROLLER_BUTTON_B;
        case SDL_CONTROLLER_BUTTON_B: return SDL_CONTROLLER_BUTTON_A;
        case SDL_CONTROLLER_BUTTON_X: return SDL_CONTROLLER_BUTTON_Y;
        case SDL_CONTROLLER_BUTTON_Y: return SDL_CONTROLLER_BUTTON_X;
        default: break;
    }
#endif
    return button;
}

static int direction_from_controller_button(Uint8 button) {
    switch (button) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return 1;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return 2;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return 3;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return 4;
        default: return 0;
    }
}

// Port: the left stick acts as the D-pad. The dominant axis wins, with hysteresis so a
// diagonal does not flicker between two directions.
static int direction_from_stick(int x,int y,int current) {
    const auto along=[&](int direction) {
        return direction==1?-y:direction==2?y:direction==3?-x:direction==4?x:0;
    };
    const int ax=std::abs(x),ay=std::abs(y);
    const int candidate=ax>ay?(x<0?3:4):(y<0?1:2);
    const int strength=std::max(ax,ay);
    if(current && along(current)>11000 && strength*4<along(current)*5) return current;
    return strength>=17000?candidate:0;
}

static Uint8 controller_button_for_direction(int direction) {
    static constexpr Uint8 buttons[]{SDL_CONTROLLER_BUTTON_DPAD_UP,SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT,SDL_CONTROLLER_BUTTON_DPAD_RIGHT};
    return buttons[std::clamp(direction,1,4)-1];
}

static SpriteTextures load_sprite_textures(SDL_Renderer* renderer, const GameArchive& archive,
                                           const std::string& stem,
                                           const std::unordered_set<std::size_t>& frame_ids,
                                           const std::vector<std::pair<std::uint32_t,std::uint32_t>>& recolors = {}) {
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

static void ensure_sprite_texture(SDL_Renderer* renderer, const GameArchive& archive,
                                  const std::string& stem, int frame_id,
                                  SpriteTextures& cache) {
    if (frame_id < 0 || cache.count(frame_id)) return;
    const auto png = load_sprite_frame_png(archive, stem,
                                           static_cast<std::size_t>(frame_id));
    const auto image = decode_indexed_png(png);
    cache.emplace(frame_id, create_image_texture(renderer, image));
}

static SpriteTextures load_terrain_textures(SDL_Renderer* renderer, const GameArchive& archive,
                                            const GameMap& map) {
    std::unordered_set<std::size_t> frame_ids;
    for (const auto frame : map.terrain) frame_ids.insert(frame);
    char stem[16];
    std::snprintf(stem, sizeof(stem), "m/t/t%02u", unsigned(map.kind));
    return load_sprite_textures(renderer, archive, stem, frame_ids);
}

static SpriteTextures load_object_textures(SDL_Renderer* renderer, const GameArchive& archive,
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

static NpcVisualAssets load_npc_textures(SDL_Renderer* renderer, const GameArchive& archive,
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

static void load_enemy_textures(SDL_Renderer* renderer, const GameArchive& archive,
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

static HeroVisualAssets load_hero_textures(SDL_Renderer* renderer,
                                           const GameArchive& archive,
                                           int character_class, const EquipmentSet& equipment, int element = 0) {
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

// Boss encounters share one /boss/{n} atlas keyed by their first stats id (ce.e(byte)).
static int boss_texture_base(int stats_id) {
    return stats_id >= 42 ? 42 : stats_id >= 39 ? 39 : stats_id >= 35 ? 35 : 32;
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

static void render(SDL_Renderer* renderer, const GameMap& map, const EventGrid& event,
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
        const auto layer=[&](const std::string& name,unsigned mask,const VisualUiModel& model) {
            SDL_SetRenderTarget(renderer,target);
            SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_NONE);
            SDL_SetRenderDrawColor(renderer,0,0,0,0);
            SDL_RenderClear(renderer);
            VisualUiModel copy=model;copy.bottom_layers=mask;
            draw_bottom_screen(renderer,copy,dialogue_textures,dialogue_font_texture,dialogue_font,true,false,hero_preview);
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

static void prepare_dialogue_font(BitmapFont& font) {recolor_bitmap_font(font,0xffffff,0x000000);}
// Russian pack: the JAR's English font plus the Russian glyphs under their real Cyrillic
// codepoints, so Latin port strings and Russian text share one font.
static BitmapFont load_text_font(GameArchive& archive,const std::string& path,void (*prepare)(BitmapFont&)) {
    const auto* pack=archive.has_language_pack()?&russian_language_pack():nullptr;
    archive.set_language_pack(nullptr);
    BitmapFont font=load_bitmap_font(archive,path);
    archive.set_language_pack(pack);
    prepare(font);
    if(pack) {
        BitmapFont russian=load_bitmap_font(archive,path);
        prepare(russian);
        add_bitmap_glyphs(font,russian,russian_font_glyphs());
        // The Russian fonts have no Й, Ё or ё (they reuse И and Е/е): draw the marks in.
        add_marked_glyph(font,0x418,0x419,false);
        add_marked_glyph(font,0x415,0x401,true);
        add_marked_glyph(font,0x435,0x451,true);
    }
    return font;
}
// The Russian bank is written in the font's ASCII encoding; decode it to real Cyrillic.
static std::vector<std::string> load_text_bank(const GameArchive& archive) {
    auto bank=load_language_bank(archive,"lang/language.fr-FR");
    if(archive.has_language_pack()) for(auto& text:bank) text=decode_russian_font_text(text);
    // The Russian translation left Rage's description (grdsk 3 -> 727) as a bare "6"; the English
    // one reads "Attacking force becomes stronger for a moment."
    if(bank.size()>727 && bank[727]=="6") {
        const std::string rage="Сила атаки ненадолго возрастает.";
        bank[727]=archive.has_language_pack()?rage:encode_russian_font_text(rage);
    }
    // gm 32 (1193) marks the active guardian; English "Equip" became the verb "Надеть" there.
    if(bank.size()>1193 && (bank[1193]=="Надеть" || bank[1193]==encode_russian_font_text("Надеть")))
        bank[1193]=archive.has_language_pack()?std::string("Надет"):encode_russian_font_text("Надет");
    // Russian lines that misdirect the player (wrong item, place or action), checked against the
    // English bank and the events (localization audit, 04.11). Applied only over the exact original.
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
    if(archive.has_language_pack())
        for(const auto& fix:fixes) if(fix.id<bank.size() && bank[fix.id]==fix.original) bank[fix.id]=fix.fixed;
    return bank;
}
// bh.d/bh.e: fonts/big.mf as it is, fill plus outline. The Russian release writes its headings
// (3920.., help topics) in the big font's own layout (MOBAR ULPA = НОВАЯ ИГРА), so that font is
// not merged with the Latin one. The fill is the rarer palette colour (the Russian font swaps them).
// font.atlas gets white fill on black outline (bh.e), black_text the inverse (bh.d).
static BitmapFont load_heading_font(GameArchive& archive,DecodedImage& black_text) {
    BitmapFont font=load_bitmap_font(archive,"fonts/big.mf");
    auto& rgba=font.atlas.rgba;
    const auto rgb_at=[&](std::size_t p){return (std::uint32_t(rgba[p])<<16)|(std::uint32_t(rgba[p+1])<<8)|rgba[p+2];};
    if(font.has_shadow_palette) {
        std::size_t fill=0,outline=0;
        for(std::size_t p=0;p+3<rgba.size();p+=4) {
            if(!rgba[p+3]) continue;
            if(rgb_at(p)==font.text_palette_rgb) ++fill; else if(rgb_at(p)==font.shadow_palette_rgb) ++outline;
        }
        if(fill>outline) std::swap(font.text_palette_rgb,font.shadow_palette_rgb);
    }
    recolor_bitmap_font(font,0xffffff,0x000000);
    black_text=font.atlas;
    for(std::size_t p=0;p+3<black_text.rgba.size();p+=4)
        if(black_text.rgba[p+3]) for(int k=0;k<3;++k) black_text.rgba[p+k]=std::uint8_t(255-black_text.rgba[p+k]);
    return font;
}
// The game JAR in a folder: an English one first (the port's Russian comes from Options),
// otherwise the first .jar by name; empty when there is none.
static std::string find_game_jar(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> jars;
    std::error_code error;
    for(const auto& entry:std::filesystem::directory_iterator(folder,error))
        if(entry.is_regular_file(error) && entry.path().extension()==".jar") jars.push_back(entry.path());
    if(jars.empty()) return {};
    std::sort(jars.begin(),jars.end());
    for(const auto& jar:jars) if(jar.filename().string().find("ENG")!=std::string::npos) return jar.string();
    return jars.front().string();
}
#if !HEROES_LORE_CONSOLE
// Desktop: the folder of the program; for an AppImage the folder of the .AppImage file
// (the program itself runs from a read-only mount).
static std::filesystem::path desktop_program_dir() {
    if(const char* appimage=std::getenv("APPIMAGE"); appimage && *appimage)
        return std::filesystem::u8path(appimage).parent_path();
    std::filesystem::path base=".";
    if(char* path=SDL_GetBasePath()) {base=std::filesystem::u8path(path);SDL_free(path);}
    return base;
}
// Desktop: the JAR beside the program.
static std::string find_desktop_jar() {
    return find_game_jar(desktop_program_dir());
}
#endif

int main(int argc, char** argv) {
#if HEROES_LORE_PSP
    const unsigned psp_initial_fpu=psp_configure_fpu();
#endif
    try {
#if HEROES_LORE_CONSOLE
        int arg = 1;
        std::string jar_path;
#if HEROES_LORE_VITA
        // Apps start at 333 MHz; the busy maps need the 444 MHz the system allows (as other ports do).
        scePowerSetArmClockFrequency(444);scePowerSetBusClockFrequency(222);
        scePowerSetGpuClockFrequency(222);scePowerSetGpuXbarClockFrequency(166);
#endif
#if HEROES_LORE_PSP
        scePowerSetClockFrequency(333,333,166);
#endif
#if HEROES_LORE_3DS
        osSetSpeedupEnable(true); // New 3DS: 804 MHz and the L2 cache; no effect on Old 3DS
#endif
        std::error_code data_dir_error; // the folder may not exist on a fresh memory card
        std::filesystem::create_directories(HEROES_LORE_DATA_DIR, data_dir_error);
        startup_log_path = HEROES_LORE_DATA_DIR "/heroes_lore.log";
        reset_startup_log();
        startup_log("visual revision: psp-hud-button-plates-v10, character/guardian setup, dialogue Next/Skip, nearest textures");
        startup_log("launch; argc=" + std::to_string(argc));
#if HEROES_LORE_PSP
        char fpu_log[100];
        std::snprintf(fpu_log,sizeof(fpu_log),"PSP FPU control: %08x -> %08x",psp_initial_fpu,psp_read_fpu_control());
        startup_log(fpu_log);
        psp_diag_reset();
#endif
        if (argc > arg && std::string(argv[arg]).size() >= 4 &&
            std::string(argv[arg]).substr(std::string(argv[arg]).size() - 4) == ".jar") {
            jar_path = argv[arg++];
        } else {
            jar_path = HEROES_LORE_DATA_DIR "/Heroes_Lore.jar";
            if (!std::filesystem::exists(jar_path)) // any other name in the data folder
                if (auto found = find_game_jar(HEROES_LORE_DATA_DIR); !found.empty()) jar_path = found;
#if HEROES_LORE_3DS
            // A .cia carries the JAR in its romfs; one on the SD card still takes precedence.
            std::error_code romfs_error;
            if (!std::filesystem::exists(jar_path) && R_SUCCEEDED(romfsInit()) &&
                std::filesystem::exists("romfs:/Heroes_Lore.jar", romfs_error)) jar_path = "romfs:/Heroes_Lore.jar";
#endif
        }
        startup_log("JAR path: " + jar_path);
#else
        int arg = 1;
        // Desktop: a .jar argument (also a JAR dropped on the program), else a .jar beside the program.
        std::string jar_path;
        if (argc > arg && std::string(argv[arg]).size() >= 4 &&
            std::string(argv[arg]).substr(std::string(argv[arg]).size() - 4) == ".jar") jar_path = argv[arg++];
        else jar_path = find_desktop_jar();
        if (jar_path.empty()) {
            const char* message =
                "Put the game's .jar (Heroes Lore: Wind of Soltia, J2ME) next to heroes_lore and start it again.\n\n"
                "Положите .jar игры (Heroes Lore: Wind of Soltia, J2ME) рядом с heroes_lore и запустите снова.";
            std::cerr << message << '\n';
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Heroes Lore Wind of Soltia", message, nullptr);
            return 2;
        }
#endif
        if (argc - arg > 3) {
            startup_log("invalid arguments; expected [game.jar] [map-id] [zone] [switches-hex]");
            return 2;
        }
#if HEROES_LORE_CONSOLE
        const std::filesystem::path save_dir=HEROES_LORE_DATA_DIR;
#else
        // Desktop: saves stay beside the JAR when that folder already holds some (earlier builds);
        // otherwise they live in the user's data folder (%APPDATA%\HeroesLore, ~/.local/share/HeroesLore).
        std::filesystem::path save_dir=std::filesystem::path(jar_path).parent_path();
        {
            const auto has_saves=[](const std::filesystem::path& dir) {
                for(const char* name:{"settings.hlc","hero6.hls","hero7.hls","hero8.hls","autosave.hls","slot1.hls"})
                    if(std::filesystem::exists(dir/name)) return true;
                return false;
            };
            if(!has_saves(save_dir)) if(char* pref=SDL_GetPrefPath("HeroesLore","HeroesLore")) {
                save_dir=std::filesystem::u8path(pref);SDL_free(pref);
            }
        }
        // Without a console (Windows) the log is the only trace of a failed start.
        startup_log_path=(save_dir/"heroes_lore.log").string();
        reset_startup_log();
        startup_log("JAR path: " + jar_path + ", saves: " + save_dir.string());
#endif
        // The original keeps one record store per hero (n.a[hero-6]); hero6/7/8.hls mirror that.
        const auto save_path_for=[&](int hero){return (save_dir/("hero"+std::to_string(hero)+".hls")).string();};
        const auto save_exists=[&](int hero){
            const auto path=save_path_for(hero);
            return std::filesystem::exists(path) || std::filesystem::exists(path+".bak");
        };
        const auto any_save_exists=[&](){return save_exists(6) || save_exists(7) || save_exists(8);};
        // Port: one autosave.hls, written on map entry; it never replaces a hero's own save.
        const auto autosave_path=(save_dir/"autosave.hls").string();
        const auto entry_path=[&](const TitleSaveEntry& entry){return entry.autosave?autosave_path:save_path_for(entry.hero);};
        {
            // Earlier port builds wrote a single slot1.hls; move it to its hero's slot.
            const auto legacy=(save_dir/"slot1.hls").string();
            if(std::filesystem::exists(legacy)) {
                try {
                    const int hero=read_save(legacy).character_class;
                    if(hero>=6 && hero<=8 && !save_exists(hero)) {
                        std::filesystem::rename(legacy,save_path_for(hero));
                        if(std::filesystem::exists(legacy+".bak")) std::filesystem::rename(legacy+".bak",save_path_for(hero)+".bak");
                    }
                } catch(const std::exception& error) {startup_log(error.what());}
            }
        }
        const bool use_new_game_spawn = argc <= arg;
        GameArchive archive(jar_path);
        // Port option: built-in Russian pack (fonts, language bank, localized images); switchable at run time.
        int session_language=0;
        {
            const auto early=read_port_settings((save_dir/"settings.hlc").string());
            const char* forced=std::getenv("HEROES_LORE_LANGUAGE"); // fixtures: HEROES_LORE_LANGUAGE=ru
            if(forced?std::string(forced)=="ru":(early.language==1 && std::getenv("HEROES_LORE_SNAPSHOT_SCREEN")==nullptr)) {
                archive.set_language_pack(&russian_language_pack());session_language=1;
            }
        }
        const auto validate_saved_resources = [&](const SaveGame& saved) {
            auto saved_map=load_map(archive,saved.map_id);
            auto saved_event=load_event_grid(archive,std::to_string(saved.zone),saved.map_id,saved_map);
            if(saved.x>=saved_map.width || saved.y>=saved_map.height)
                throw std::runtime_error("saved position outside map");
            const auto catalog=load_item_catalog(archive);
            const auto check_item=[&](const InventoryStack& item) {
                if(!find_catalog_item(catalog,item.item_type,item.item_id))
                    throw std::runtime_error("saved item does not exist in JAR");
            };
            for(const auto& item:saved.inventory) check_item(item);
            for(const auto& item:saved.equipment) if(item) check_item(*item);
        };
        std::optional<SaveGame> startup_save;
        std::string startup_notice;
        // Title screen entries (n.p): hero, level, progress for every hero with a readable save.
        std::vector<TitleSaveEntry> title_saves;
        int shown_level=0; // last hero level seen by the renderer; a rise plays the ao.r level-up effects
        const auto scan_saves=[&](){
            title_saves.clear();
            if(std::filesystem::exists(autosave_path) || std::filesystem::exists(autosave_path+".bak")) {
                try {
                    const auto saved=read_save(autosave_path);
                    title_saves.push_back({saved.character_class,saved.progress.level,save_progress_percent(saved),true});
                } catch(const std::exception& error) {startup_log(error.what());}
            }
            for(int hero=6;hero<=8;++hero) {
                if(!save_exists(hero)) continue;
                try {
                    const auto saved=read_save(save_path_for(hero));
                    title_saves.push_back({hero,saved.progress.level,save_progress_percent(saved)});
                } catch(const std::exception& error) {startup_log(error.what());}
            }
        };
        scan_saves();
        if(use_new_game_spawn && !title_saves.empty()) {
            try {
                // Preload the first saved hero so the title screen opens on familiar resources.
                bool backup=false;auto saved=read_save(entry_path(title_saves.front()),&backup);validate_saved_resources(saved);
                startup_save=std::move(saved);
                startup_notice=backup?"Backup save is available.":"";
            } catch(const std::exception& error) {
                startup_notice="The save could not be loaded. New game is available.";startup_log(error.what());
            }
        }
        auto new_game = make_new_game_profile(startup_save?startup_save->character_class:6);
        int current_map_id = startup_save?startup_save->map_id:argc > arg ? std::stoi(argv[arg++]) : new_game.map_id;
        std::string current_zone = startup_save?std::to_string(startup_save->zone):argc > arg ? argv[arg++] : "6";
        const bool has_switches = argc > arg;
        startup_log("JAR opened");
        GameMap map = load_map(archive, current_map_id);
        startup_log("map loaded: " + std::to_string(current_map_id));
        EventGrid event_grid = load_event_grid(archive, current_zone, current_map_id, map);
        startup_log("event loaded: zone " + current_zone);
        auto language_bank = load_text_bank(archive);
        // Port-only text in the active language (UTF-8; the fonts carry native Cyrillic).
        const auto tr=[&](const char* en,const char* ru){return archive.has_language_pack()?std::string(ru):std::string(en);};
        const auto bank_or=[&](std::size_t i,const char* fallback){return i<language_bank.size()?language_bank[i]:std::string(fallback);};
        auto shop_ui_text=parse_tdf_text(archive.read_entry("sgui/shop.tdf"),language_bank); // bp.a strings
        auto help_text =parse_tdf_text(archive.read_entry("sgui/help.tdf"),language_bank);
        auto menu_text =parse_tdf_text(archive.read_entry("sgui/gm.tdf"),language_bank);
        auto common_text =parse_tdf_text(archive.read_entry("sgui/com.tdf"),language_bank);
        auto blacksmith_text=parse_tdf_text(archive.read_entry("sgui/blak.tdf"),language_bank); // aa.a
        auto refiner_text=parse_tdf_text(archive.read_entry("sgui/refi.tdf"),language_bank);    // ax.a
        auto attribute_text=parse_tdf_text(archive.read_entry("itm/itmatt.tdf"),language_bank); // t.a: option names
        auto item_type_text=parse_tdf_text(archive.read_entry("itm/itmtp.tdf"),language_bank);  // ad.b: type names
        std::string about_text=language_bank[3905]+"\n\n"+language_bank[3906];
        auto hero_text = parse_tdf_text(archive.read_entry("char/hero.tdf"),language_bank);
        auto guardian_text = parse_tdf_text(archive.read_entry("grd/grd.tdf"),language_bank);
        auto location_names = parse_tdf_text(archive.read_entry("m/name.tdf"),language_bank);
        auto quest_text = parse_tdf_text(archive.read_entry("sgui/q"+
            std::to_string(new_game.character_class)+".tdf"),language_bank);
        const auto location_name_for = [&](int id) -> std::string {
            if(new_game.character_class==8 && id==65) id=85; // ae.a special name.
            return id>=0 && std::size_t(id)<location_names.size()?location_names[id]:"";
        };
        auto initial_switches = startup_save?startup_save->switches:new_game.switches;
        if (has_switches) {
            const auto supplied = parse_switch_hex(argv[arg]);
            std::fill(initial_switches.begin(), initial_switches.end(), 0);
            std::copy(supplied.begin(), supplied.end(), initial_switches.begin());
        }
        const auto setup = select_setup_group(event_grid, initial_switches);
        if (setup) apply_setup_group(map, event_grid, setup->second);
        auto objects = decode_objects(event_grid);
        auto npcs = decode_npcs(event_grid);
        auto enemies = decode_enemies(event_grid);
        auto npc_motion = make_npc_motion_states(event_grid);
        HeroInput hero_input(map, event_grid, npcs, enemies);
        if (use_new_game_spawn) {
            hero_input.place_on_tile(startup_save?startup_save->x:new_game.x,startup_save?startup_save->y:new_game.y);
            hero_input.apply_script_command(6,static_cast<std::uint8_t>(startup_save?startup_save->direction:new_game.direction));
        }
        const auto spawn = hero_input.state();
        startup_log(std::string(use_new_game_spawn ? "new game hero spawn: " : "debug hero spawn: ") + std::to_string(spawn.x_tile) + "," +
                    std::to_string(spawn.y_tile));
#if HEROES_LORE_SWITCH
        // Fix the input contract: positional SDL values, then translate to labels.
        SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0", SDL_HINT_OVERRIDE);
#endif
        SDL_SetHintWithPriority(SDL_HINT_RENDER_SCALE_QUALITY,"0",SDL_HINT_OVERRIDE);
        if (SDL_Init(SDL_INIT_VIDEO) != 0) throw std::runtime_error(SDL_GetError());
        AudioMixer audio;
        AudioDevice audio_device;
        bool skip_audio=false;
#if HEROES_LORE_PSP
        skip_audio=std::filesystem::exists(HEROES_LORE_DATA_DIR "/no-audio.txt");
        startup_log(skip_audio?"PSP audio disabled by no-audio.txt":"PSP audio: embedded compact SoundFont, 22050 Hz synth, 44100 Hz output, 24 sample voices per slot");
        startup_log("PSP controls: native polling");
#endif
        if (!skip_audio && !std::getenv("HEROES_LORE_SNAPSHOT_SCREEN") && SDL_InitSubSystem(SDL_INIT_AUDIO) == 0) {
            SDL_AudioSpec want{};want.freq=HEROES_LORE_LOW_POWER?44100:AudioMixer::kRate;want.format=AUDIO_S16SYS;want.channels=2;
            want.samples=HEROES_LORE_LOW_POWER?2048:1024;want.callback=audio_callback;want.userdata=&audio;
            audio_device.id=SDL_OpenAudioDevice(nullptr,0,&want,nullptr,0);
            if(audio_device.id) SDL_PauseAudioDevice(audio_device.id,0);
#if HEROES_LORE_3DS
            // SDL puts its audio thread on the system core with a 30% limit: too little for the synth.
            if(audio_device.id) {
                g_3ds_audio_core=R_SUCCEEDED(APT_SetAppCpuTimeLimit(80));
                startup_log(g_3ds_audio_core?"3DS audio core limit: 80%":"3DS audio core limit: unchanged");
            }
            startup_log("3DS memory: heap "+std::to_string(envGetHeapSize()>>10)+" KB, linear "+std::to_string(envGetLinearHeapSize()>>10)+
                " KB, linear free "+std::to_string(linearSpaceFree()>>10)+" KB");
            if(audio_device.id) {g_3ds_audio=audio_device.id;aptHook(&g_3ds_apt_cookie,apt_hook_3ds,nullptr);}
#endif
            else startup_log("audio device unavailable: "+std::string(SDL_GetError()));
        } else if (!std::getenv("HEROES_LORE_SNAPSHOT_SCREEN")) startup_log("audio unavailable: " + std::string(SDL_GetError()));
        // Port option: a General MIDI .sf2 next to the game (GeneralUser GS preferred) replaces the
        // built-in synth for music. It is read on the main thread once the title is up: on Switch a
        // worker thread crashed when it exited (libnx threadExit -> eh_globals_dtor).
#if HEROES_LORE_LOW_POWER
        // PSP / 3DS: the built-in compressed SoundFont is the only one, read before the title.
        if(audio_device.id) {
            const void* embedded=nullptr;std::size_t embedded_size=0;
#if HEROES_LORE_3DS
            // Old 3DS: sample playback is too heavy for the main core; without the system core the
            // music stays on the built-in synth (New 3DS has the speed either way).
            bool new_3ds=false;APT_CheckNew3DS(&new_3ds);
            if(!new_3ds) audio.set_soundfont_voices(12); // half the voices: Old 3DS's system core also serves GSP/HID
            if(!new_3ds && !g_3ds_audio_core) {startup_log("Embedded SoundFont: skipped, Old 3DS without the system core");embedded_size=0;}
            else
#endif
            if(embedded_soundfont(&embedded,&embedded_size)) {
                const Uint32 load_started=SDL_GetTicks();
                const bool loaded=audio.load_soundfont_compressed(embedded,embedded_size);
#if HEROES_LORE_PSP
                psp_diag_stage(loaded?"embedded-font-loaded":"embedded-font-failed",int(embedded_size),int(SDL_GetTicks()-load_started));
#endif
                startup_log("Embedded SoundFont: "+std::string(loaded?"loaded, compressed bytes=":"FAILED, builtin fallback, bytes=")+
                    std::to_string(embedded_size)+", ms="+std::to_string(SDL_GetTicks()-load_started));
            } else startup_log("Embedded SoundFont: none in this build, builtin synth");
        }
#endif
#if HEROES_LORE_PSP
        startup_log("PSP render cap: 30 FPS; game tick clock retains frame remainder");
#endif
        std::string soundfont;
        if(audio_device.id && !HEROES_LORE_LOW_POWER) {
            // Desktop saves live in the user's data folder, so also look beside the program and the JAR.
            std::vector<std::filesystem::path> folders{save_dir};
#if !HEROES_LORE_CONSOLE
            folders.push_back(desktop_program_dir());
            folders.push_back(std::filesystem::path(jar_path).parent_path());
#endif
            bool general_user=false;
            for(const auto& folder:folders) {
                std::error_code error;
                for(const auto& entry:std::filesystem::directory_iterator(folder.empty()?std::filesystem::path("."):folder,error)) {
                    std::string lower=entry.path().filename().string();
                    std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return char(std::tolower(c));});
                    if(lower.size()<5 || lower.compare(lower.size()-4,4,".sf2")!=0) continue;
                    const bool preferred=lower.find("generaluser")!=std::string::npos;
                    if(soundfont.empty() || (preferred && !general_user)) {soundfont=entry.path().string();general_user=preferred;}
                }
            }
            // Without one, the SoundFont built into the program (when this build has it).
            const void* embedded=nullptr;std::size_t embedded_size=0;
            if(soundfont.empty() && embedded_soundfont(&embedded,&embedded_size)) soundfont=kEmbeddedSoundFont;
            startup_log(soundfont.empty()?std::string("SoundFont: none found"):"SoundFont: "+soundfont);
        }
#if !HEROES_LORE_PSP
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
            startup_log("game controller subsystem unavailable: " + std::string(SDL_GetError()));
        else
            SDL_GameControllerEventState(SDL_ENABLE);
#endif
        startup_log("SDL video initialized");
        int window_w=1280,window_h=720;
#if !HEROES_LORE_CONSOLE
        if(const char* width=std::getenv("HEROES_LORE_SNAPSHOT_WIDTH")) window_w=std::stoi(width);
        if(const char* height=std::getenv("HEROES_LORE_SNAPSHOT_HEIGHT")) window_h=std::stoi(height);
#endif
#if HEROES_LORE_CONSOLE
        SDL_DisplayMode native_mode{};
        if(SDL_GetCurrentDisplayMode(0,&native_mode)==0 && native_mode.w>0 && native_mode.h>0) {
            window_w=native_mode.w;window_h=native_mode.h;
        }
#endif
        SDL_Window* window = SDL_CreateWindow("Heroes Lore Wind of Soltia",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, window_w, window_h, SDL_WINDOW_RESIZABLE);
        if (!window) { SDL_Quit(); throw std::runtime_error(SDL_GetError()); }
#if !HEROES_LORE_CONSOLE
        // Handhelds without a desktop (KMSDRM, e.g. PortMaster): the whole panel, whatever its size
        // (640x480, 720x720...); a desktop window keeps a usable minimum.
        if(const char* video=SDL_GetCurrentVideoDriver(); video && std::string(video)=="KMSDRM")
            SDL_SetWindowFullscreen(window,SDL_WINDOW_FULLSCREEN_DESKTOP);
        else if(!std::getenv("HEROES_LORE_SNAPSHOT_WIDTH")) SDL_SetWindowMinimumSize(window,960,540); // snapshots may be smaller
#endif
#if HEROES_LORE_VITA
        // GLES2 through vitaGL (needs ur0:data/libshacccg.suprx, as other vitaGL ports do): SDL's own
        // "VITA gxm" renderer drew only the first glyph of each text run and ran at 2 FPS.
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengles2");
        // Naming a driver turns SDL's draw batching off by default; without it every tile and glyph
        // is its own draw call (25-60 FPS on busy maps).
        SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");
#endif
#if HEROES_LORE_3DS
        g_3ds_frame=SDL_CreateRGBSurfaceWithFormat(0,window_w,window_h,32,SDL_PIXELFORMAT_ARGB8888);
#ifndef HEROES_LORE_3DS_GPU
        // The bottom screen changes rarely and in small parts (the hero's minimap cell): one buffer,
        // written in place, instead of two that would each need the whole picture.
        gfxSetDoubleBuffering(GFX_BOTTOM,false);
#endif
        SDL_Renderer* renderer = g_3ds_frame?SDL_CreateSoftwareRenderer(g_3ds_frame):nullptr;
        const bool renderer_has_vsync = false;
#ifdef HEROES_LORE_3DS_GPU
        // The SDL renderer only stands in as a handle: every render call goes to citro2d (gpu_3ds.cpp).
        if(!hl3d::init()) throw std::runtime_error("citro2d init failed");
        startup_log("3DS renderer: citro2d GPU, stereo 3D");
#endif
#else
        SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        const bool renderer_has_vsync = renderer != nullptr;
#endif
        if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        if (!renderer) { SDL_DestroyWindow(window); SDL_Quit(); throw std::runtime_error(SDL_GetError()); }
        SDL_RenderSetScale(renderer,1.0f,1.0f);
        SDL_RendererInfo renderer_info{};
        if (SDL_GetRendererInfo(renderer, &renderer_info) == 0)
            startup_log("renderer: " + std::string(renderer_info.name ? renderer_info.name : "unknown") +
                        ", flags=" + std::to_string(renderer_info.flags));
        SDL_GameController* controller = nullptr;
#if !HEROES_LORE_PSP
        for (int i = 0; i < SDL_NumJoysticks() && !controller; ++i)
            if (SDL_IsGameController(i)) controller = SDL_GameControllerOpen(i);
#endif
        // C4: hints follow the last device used (a pad's face-button labels, or the keyboard).
        const auto pad_style = [](SDL_GameController* pad) {
            switch (pad ? SDL_GameControllerGetType(pad) : SDL_CONTROLLER_TYPE_UNKNOWN) {
                case SDL_CONTROLLER_TYPE_PS3: case SDL_CONTROLLER_TYPE_PS4: case SDL_CONTROLLER_TYPE_PS5:
                    return ButtonStyle::playstation;
                case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return ButtonStyle::nintendo;
                default: return ButtonStyle::xbox;
            }
        };
#if HEROES_LORE_CONSOLE
        ButtonStyle button_style = (HEROES_LORE_VITA || HEROES_LORE_PSP) ? ButtonStyle::playstation : ButtonStyle::nintendo;
#else
        ButtonStyle button_style = controller ? pad_style(controller) : ButtonStyle::keyboard;
        if (const char* forced = std::getenv("HEROES_LORE_BUTTONS")) { // snapshots: keyboard/xbox/nintendo/playstation
            const std::string name = forced;
            if (name == "xbox") button_style = ButtonStyle::xbox;
            else if (name == "nintendo") button_style = ButtonStyle::nintendo;
            else if (name == "playstation") button_style = ButtonStyle::playstation;
        }
#endif
        SpriteTextures terrain_textures, object_textures, dialogue_textures;
        NpcVisualAssets npc_assets;
        HeroVisualAssets hero_assets;
        BitmapFont dialogue_font, startup_heading_font;
        SDL_Texture* dialogue_font_texture = nullptr;
        BitmapFont latin_font;SDL_Texture* latin_font_texture=nullptr;
        try {
            terrain_textures = load_terrain_textures(renderer, archive, map);
            object_textures = load_object_textures(renderer, archive, map, event_grid, objects);
            npc_assets = load_npc_textures(renderer, archive, npcs);
            load_enemy_textures(renderer, archive, enemies, npc_assets);
            hero_assets = load_hero_textures(renderer, archive, new_game.character_class,
                startup_save?startup_save->equipment:new_game.equipment);
            dialogue_font = load_text_font(archive, "fonts/small.mf", prepare_dialogue_font);
            dialogue_font_texture = create_image_texture(renderer, dialogue_font.atlas);
            {
                // The JAR's own font keeps Latin glyphs for port-only strings in any language.
                const auto* pack=archive.has_language_pack()?&russian_language_pack():nullptr;
                archive.set_language_pack(nullptr);
                latin_font=load_bitmap_font(archive,"fonts/small.mf");
                archive.set_language_pack(pack);
                recolor_bitmap_font(latin_font,0xffffff,0x000000);
                latin_font_texture=create_image_texture(renderer,latin_font.atlas);
            }
            dialogue_textures = load_sprite_textures(renderer,archive,"img/uifrm",{0,1,2,3,4,5,6,7,8});
            {
                DecodedImage black_text;
                startup_heading_font=load_heading_font(archive,black_text);
                dialogue_textures.emplace(850,create_image_texture(renderer,startup_heading_font.atlas));
                dialogue_textures.emplace(851,create_image_texture(renderer,black_text));
            }
            const auto faces = load_sprite_atlas(archive,"m/face");
            std::unordered_set<std::size_t> face_ids;
            for (std::size_t i = 0; i < faces.frames.size(); ++i) face_ids.insert(i);
            auto portrait_textures = load_sprite_textures(renderer,archive,"m/face",face_ids);
            for (const auto& entry : portrait_textures) dialogue_textures.emplace(100+entry.first,entry.second);
            const auto icons = load_sprite_atlas(archive,"img/icoitm");
            std::unordered_set<std::size_t> icon_ids;
            for(std::size_t i=0;i<icons.frames.size();++i) icon_ids.insert(i);
            auto item_icons=load_sprite_textures(renderer,archive,"img/icoitm",icon_ids);
            for(const auto& entry:item_icons) dialogue_textures.emplace(200+entry.first,entry.second);
            auto menu_icons=load_sprite_textures(renderer,archive,"sgui/gmico",{0,1,2,3,4,6,7,8,9,10,11});
            for(const auto& entry:menu_icons) dialogue_textures.emplace(300+entry.first,entry.second);
            const auto guardian_atlas=load_sprite_atlas(archive,"grd/grdico");
            std::unordered_set<std::size_t> guardian_frames;
            for(std::size_t i=0;i<guardian_atlas.frames.size();++i) guardian_frames.insert(i);
            auto guardian_icons=load_sprite_textures(renderer,archive,"grd/grdico",guardian_frames);
            for(const auto& entry:guardian_icons) dialogue_textures.emplace(500+entry.first,entry.second);
            // ce.C: img/etcui 11 covers a guardian skill icon while its cooldown runs (as.a).
            for(const auto& entry:load_sprite_textures(renderer,archive,"img/etcui",{11})) dialogue_textures.emplace(0x52020,entry.second);
            // ce.w: img/logo; bg draws frame 4 during the startup splash.
            for(const auto& entry:load_sprite_textures(renderer,archive,"img/logo",{4})) dialogue_textures.emplace(0x52010,entry.second);
            auto title_images=load_sprite_textures(renderer,archive,"img/title1",{0,2,3,4});
            for(const auto& entry:title_images) dialogue_textures.emplace(600+entry.first,entry.second);
            // ce.A loads the original startup parchment and brush atlas.
            const auto startup_atlas=load_sprite_atlas(archive,"sgui/mm/etc");
            std::unordered_set<std::size_t> startup_frames;
            for(std::size_t i=0;i<startup_atlas.frames.size();++i) startup_frames.insert(i);
            auto startup_images=load_sprite_textures(renderer,archive,"sgui/mm/etc",startup_frames);
            for(const auto& entry:startup_images) dialogue_textures.emplace(800+entry.first,entry.second);
            // ce.r: sgui/shop tab icons 0..5, 6 worse / 7 better markers; localized _sgui_shop__8/9.
            for(const auto& entry:load_sprite_textures(renderer,archive,"sgui/shop",{0,1,2,3,4,5,6,7}))
                dialogue_textures.emplace(0x51000+int(entry.first),entry.second);
            for(int frame:{8,9}) {
                const std::string path="fr-FR/_sgui_shop__"+std::to_string(frame)+".png";
                if(archive.contains(path))
                    dialogue_textures.emplace(0x51000+frame,create_image_texture(renderer,decode_indexed_png(archive.read_entry(path))));
            }
            auto feathers=load_sprite_textures(renderer,archive,"img/title2",{0,1,2,3,4});
            for(const auto& entry:feathers) dialogue_textures.emplace(650+entry.first,entry.second);
            for(int i=0;i<3;++i) {
                const auto png=load_sprite_frame_png(archive,"sgui/mm/face",i);
                auto face=decode_indexed_png(png);dialogue_textures.emplace(700+i,create_image_texture(renderer,face));
                if(i==0) {
                    // ce: img/glb 0/1 arrows, 5 digits (r.c), 6/8 status tab art, 7 coin, 14 slash (r.d); localized
                    // fr-FR/_img_glb__N icons EXP/HP/LV/NEXT/?/SP. Keys 0x50000+N.
                    for(const auto& entry:load_sprite_textures(renderer,archive,"img/glb",{0,1,2,3,5,6,7,8,14}))
                        dialogue_textures.emplace(0x50000+int(entry.first),entry.second);
                    for(int icon:{9,10,11,12,13,15}) {
                        const std::string path="fr-FR/_img_glb__"+std::to_string(icon)+".png";
                        if(!archive.contains(path)) continue;
                        auto image=decode_indexed_png(archive.read_entry(path));
                        dialogue_textures.emplace(0x50000+icon,create_image_texture(renderer,image));
                    }
                }
                for(std::size_t pixel=0;pixel<face.rgba.size();pixel+=4) {
                    const auto gray=std::uint8_t((int(face.rgba[pixel])*30+int(face.rgba[pixel+1])*59+int(face.rgba[pixel+2])*11)/100);
                    face.rgba[pixel]=face.rgba[pixel+1]=face.rgba[pixel+2]=gray;
                }
                dialogue_textures.emplace(710+i,create_image_texture(renderer,face));
                auto guardian=load_sprite_textures(renderer,archive,"grd/"+std::to_string(i),{0,1});
                for(const auto& entry:guardian) dialogue_textures.emplace(720+i*2+entry.first,entry.second);
            }
            {   // ce.f(id): element atlases grd/fi, grd/wa, grd/gr (all frames) for skill effects
                static const char* atlases[3]{"grd/fi","grd/wa","grd/gr"};
                for(int a=0;a<3;++a) {
                    const auto atlas=load_sprite_atlas(archive,atlases[a]);
                    std::unordered_set<std::size_t> frames;
                    for(std::size_t f=0;f<atlas.frames.size();++f) frames.insert(f);
                    for(const auto& entry:load_sprite_textures(renderer,archive,atlases[a],frames))
                        dialogue_textures.emplace(guardian_fx_key(a,int(entry.first)),entry.second);
                }
            }
            for(int i=3;i<6;++i) { // event guardians 3..5 (field summons)
                auto guardian=load_sprite_textures(renderer,archive,"grd/"+std::to_string(i),{0,1});
                for(const auto& entry:guardian) dialogue_textures.emplace(720+i*2+entry.first,entry.second);
            }
            startup_log("textures loaded; all textures use nearest filtering");
        } catch (...) {
            destroy_textures(terrain_textures);
            destroy_textures(dialogue_textures);
            destroy_textures(object_textures);
            destroy_textures(npc_assets.textures);
            destroy_textures(npc_assets.enemy_textures);
            destroy_textures(hero_assets.textures);
            if (dialogue_font_texture) SDL_DestroyTexture(dialogue_font_texture);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            throw;
        }
        startup_log("loaded map " + std::to_string(current_map_id) + " zone " + current_zone + ": kind=" +
                    std::to_string(map.kind) + ", " + std::to_string(map.width) + "x" +
                    std::to_string(map.height));
        startup_log("controls: A interact/confirm, B cancel, X page, Y overlay, Plus pause");
        startup_log("baseline switch0=" + std::to_string(initial_switches[0] & 1u) +
                    ", setup=" + std::to_string(setup ? setup->first : -1));
        if (controller) startup_log("controller=" + std::string(SDL_GameControllerName(controller)) +
                                    ", rumble=" + std::to_string(int(SDL_GameControllerHasRumble(controller))));
        // Port rumble; the first few results go to the log to diagnose pads that stay silent.
        int rumble_logged=0;
#if HEROES_LORE_SWITCH
        SwitchRumble switch_rumble;
        const auto pad_rumble=[&](Uint16 low,Uint16 high,Uint32 ms,const char* why) {
            const auto info=switch_rumble.pulse(low/65535.0f,high/65535.0f,ms,SDL_GetTicks());
            if(rumble_logged<8) {++rumble_logged;startup_log(std::string("rumble ")+why+": "+info);}
        };
#else
        const auto pad_rumble=[&](Uint16 low,Uint16 high,Uint32 ms,const char* why) {
            if(!controller) {if(rumble_logged<3) {++rumble_logged;startup_log(std::string("rumble ")+why+": no controller");}return;}
            const int result=SDL_GameControllerRumble(controller,low,high,ms);
            if(rumble_logged<8) {++rumble_logged;startup_log(std::string("rumble ")+why+": "+(result==0?std::string("ok"):"failed: "+std::string(SDL_GetError())));}
        };
#endif
        startup_log("entering render loop");
        bool running = true;
        SDL_Event sdl_event{};
        int logged_events = 0;
        bool first_frame = true;
        bool show_collision = false;
        std::vector<std::uint8_t> persistent_switches = initial_switches;
        auto persistent_quests = startup_save?startup_save->quests:new_game.quests;
        auto persistent_recipes = startup_save?startup_save->recipes:std::vector<std::uint8_t>{}; // port recipe journal
        std::vector<InventoryStack> persistent_inventory=startup_save?startup_save->inventory:std::vector<InventoryStack>{};
        std::int64_t persistent_money = startup_save?startup_save->money:new_game.money;
        auto persistent_guardians = make_guardian_roster(new_game.guardian_choices);
        int quick_item_type=7;
        HeroProgress persistent_progress;
        persistent_progress.attributes=new_game.base_attributes;
        HeroVitals persistent_vitals;
        persistent_vitals.hp=persistent_vitals.max_hp=(new_game.base_attributes[1]+1)*12;
        persistent_vitals.sp=persistent_vitals.max_sp=(new_game.base_attributes[3]+1)*12;
        if(startup_save) {
            persistent_guardians=startup_save->guardians;persistent_progress=startup_save->progress;
            persistent_vitals=startup_save->vitals;quick_item_type=startup_save->quick_item_type;
        }
        auto event_visuals = make_event_visual_state(event_grid);
        Uint32 location_started = SDL_GetTicks();
        std::unique_ptr<EventRuntime> event_runtime;
        std::vector<std::uint8_t> active_action_storage;
        const std::vector<std::uint8_t>* active_actions = nullptr;
        bool screen_return_active = false;
        bool menu_requested = false;
        bool interaction_requested = false;
        bool menu_active = false;
        bool minimap_open = false; // as state 11
        bool minimap_lit = false;  // ae.d
        Uint32 minimap_tick = 0;
        bool autosave_pending = false; // set on map entry, written once the hero stands free
        int menu_selection = 0;
        int menu_page = 0; // Status, Item, Equipment, Guardian, Quest, Save.
        std::size_t equipment_selection = 0;
        std::size_t guardian_selection = 0;
        // bm: A opens the Recall / Skill choice (-1 closed); bn: skill keys 1 (calm) / 3 (wild).
        int guardian_choice=-1;
        bool skill_screen=false,skill_key_calm=true,attribute_confirming=false;
        int skill_selection=0;
        // ay: af dialog for the selected bag item -- 4 Equip/Trash, 5 Use/Trash, 6 trash confirmation.
        int item_action=0,item_choice=0;
        std::string item_prompt;
        std::size_t quest_selection = 0;
        std::size_t recipe_selection = 0;
        bool recipes_view = false; // port: Quest tab shows the recipe journal (Y toggles)
        bool quest_reading = false;
        bool attribute_editing = false;
        int attribute_selection = 0;
        std::array<int,4> pending_attributes{};
        std::size_t bag_selection = 0;
        bool equipment_confirming = false;
        bool unequip_confirming = false; // port: Equipment tab, A on a worn piece
        bool tab_focus = false; // port Modern menus: the cursor is in the tab column (menu and shop)
        bool modern_layout = false; // mirrors Options Menus = Modern, refreshed every frame
        std::string equipment_notice;
        auto equipped_items = startup_save?startup_save->equipment:new_game.equipment;
        int save_selection=0;
        bool save_confirming=false;
        bool save_available=any_save_exists();
        bool shop_active = false;
        std::vector<ShopItem> shop_catalog;
        std::vector<ShopItem> item_catalog;
        // n.g: Hardcore runs after an ending; ao.f: progress banked by earlier clears.
        int persistent_new_game_plus=startup_save?startup_save->new_game_plus:0;
        int persistent_clear_progress=startup_save?startup_save->clear_progress:0;
        // Field combat: ae reads /enm/data<g>, g capped at 2.
        const auto enemy_table_path=[](int new_game_plus){return "enm/data"+std::to_string(std::min(new_game_plus,2));};
        std::vector<combat::EnemyStats> enemy_stats;
        try { enemy_stats = combat::parse_enemy_table(archive.read_entry(enemy_table_path(persistent_new_game_plus))); }
        catch (const std::exception& error) { startup_log(std::string("enemy table load failed: ") + error.what()); }
        combat::CombatWorld combat_world;
        std::vector<MapEntity> enemy_spawn_records;
        SpriteTextures combat_textures; // img/etcui: 1 MISS, 3 shadow, 9 item drop, 10 gold drop
        try { combat_textures = load_sprite_textures(renderer, archive, "img/etcui", {1, 3, 5, 6, 7, 8, 9, 10}); }
        catch (const std::exception& error) { startup_log(std::string("combat textures failed: ") + error.what()); }
        for(const auto& entry:load_sprite_textures(renderer,archive,"img/keepst",{0,1,2,3,4,5,6,7,8}))
            combat_textures.emplace(0x300+int(entry.first),entry.second);
        std::array<EifAnimation,4> death_effects;
        // ce: char/lvup atlas + lvup.eif (aw4), localized _img_etcui__0 LEVEL UP caption (aw3, ce.s).
        try {
            death_effects[3]=load_eif(archive,"char/lvup.eif");
            const auto atlas=load_sprite_atlas(archive,"char/lvup");
            std::unordered_set<std::size_t> frames;
            for(std::size_t i=0;i<atlas.frames.size();++i) frames.insert(i);
            for(const auto& entry:load_sprite_textures(renderer,archive,"char/lvup",frames))
                combat_textures.emplace(0x400+int(entry.first),entry.second);
            if(archive.contains("fr-FR/_img_etcui__0.png"))
                combat_textures.emplace(0x3ff,create_image_texture(renderer,decode_indexed_png(archive.read_entry("fr-FR/_img_etcui__0.png"))));
        } catch(const std::exception& error) {startup_log(std::string("level-up effect load failed: ")+error.what());}
        for(int bank=0;bank<3;++bank) {
            const auto images=load_sprite_textures(renderer,archive,"img/atteff"+std::to_string(bank+1),{0,1,2});
            for(const auto& entry:images) combat_textures.emplace(0x100+bank*16+int(entry.first),entry.second);
            death_effects[bank]=load_eif(archive,"enm/die/"+std::to_string(bank));
            combat_world.set_death_effect_frames(bank,int(death_effects[bank].size()));
        }
        const auto death_atlas=load_sprite_atlas(archive,"enm/die/bang");
        std::unordered_set<std::size_t> death_frames;
        for(std::size_t i=0;i<death_atlas.frames.size();++i) death_frames.insert(i);
        for(const auto& entry:load_sprite_textures(renderer,archive,"enm/die/bang",death_frames))
            combat_textures.emplace(0x200+int(entry.first),entry.second);
        Uint32 game_over_until{};
        int game_over_frames{};   // as.e: draws left on the game-over screen (16 .. 1)
        GuardianFx guardian_fx;
        for (int id = 0; id < 6; ++id) {
            auto& fx = guardian_fx[std::size_t(id)];
            fx.atlas = id % 3;
            for (int slot = 0; slot < 3; ++slot) {
                const std::string path = id == 0 && slot == 0 ? "grd/spr/0_01.eif"
                    : "grd/spr/" + std::to_string(id) + "_" + std::to_string(slot) + ".eif";
                if (archive.contains(path)) fx.slots[std::size_t(slot)] = load_eif(archive, path);
            }
            if (id == 0 && archive.contains("grd/spr/0_02.eif")) fx.extra = load_eif(archive, "grd/spr/0_02.eif");
        }
        std::vector<std::string> guardian_skill_text;
        try { guardian_skill_text = parse_tdf_text(archive.read_entry("grd/grdsk.tdf"), language_bank); }
        catch (const std::exception& error) { startup_log(std::string("grdsk.tdf failed: ") + error.what()); }
        // Options -> Language: swap the built-in pack and reload every language-dependent resource.
        const auto replace_texture=[](auto& textures,int key,SDL_Texture* texture) {
            const auto found=textures.find(key);
            if(found!=textures.end()) {SDL_DestroyTexture(found->second);found->second=texture;}
            else textures.emplace(key,texture);
        };
        // ce.t: localized MISS (aw) replaces the etcui atlas frame; ce.v: STAT plate (as).
        if(archive.contains("fr-FR/_img_etcui__1.png"))
            replace_texture(combat_textures,1,create_image_texture(renderer,decode_indexed_png(archive.read_entry("fr-FR/_img_etcui__1.png"))));
        if(archive.contains("fr-FR/_img_etcui__4.png"))
            replace_texture(dialogue_textures,0x51010,create_image_texture(renderer,decode_indexed_png(archive.read_entry("fr-FR/_img_etcui__4.png"))));
        const auto reload_language=[&](int language) {
            try {
                archive.set_language_pack(language==1?&russian_language_pack():nullptr);
                session_language=language;
                language_bank=load_text_bank(archive);
                help_text=parse_tdf_text(archive.read_entry("sgui/help.tdf"),language_bank);
                shop_ui_text=parse_tdf_text(archive.read_entry("sgui/shop.tdf"),language_bank);
                for(int frame:{8,9}) {
                    const std::string path="fr-FR/_sgui_shop__"+std::to_string(frame)+".png";
                    if(archive.contains(path))
                        replace_texture(dialogue_textures,0x51000+frame,create_image_texture(renderer,decode_indexed_png(archive.read_entry(path))));
                }
                menu_text=parse_tdf_text(archive.read_entry("sgui/gm.tdf"),language_bank);
                common_text=parse_tdf_text(archive.read_entry("sgui/com.tdf"),language_bank);
                blacksmith_text=parse_tdf_text(archive.read_entry("sgui/blak.tdf"),language_bank);
                refiner_text=parse_tdf_text(archive.read_entry("sgui/refi.tdf"),language_bank);
                attribute_text=parse_tdf_text(archive.read_entry("itm/itmatt.tdf"),language_bank);
                item_type_text=parse_tdf_text(archive.read_entry("itm/itmtp.tdf"),language_bank);
                about_text=language_bank[3905]+"\n\n"+language_bank[3906];
                hero_text=parse_tdf_text(archive.read_entry("char/hero.tdf"),language_bank);
                guardian_text=parse_tdf_text(archive.read_entry("grd/grd.tdf"),language_bank);
                location_names=parse_tdf_text(archive.read_entry("m/name.tdf"),language_bank);
                quest_text=parse_tdf_text(archive.read_entry("sgui/q"+std::to_string(new_game.character_class)+".tdf"),language_bank);
                guardian_skill_text=parse_tdf_text(archive.read_entry("grd/grdsk.tdf"),language_bank);
                dialogue_font=load_text_font(archive,"fonts/small.mf",prepare_dialogue_font);
                if(dialogue_font_texture) SDL_DestroyTexture(dialogue_font_texture);
                dialogue_font_texture=create_image_texture(renderer,dialogue_font.atlas);
                {
                    DecodedImage black_text;
                    startup_heading_font=load_heading_font(archive,black_text);
                    replace_texture(dialogue_textures,850,create_image_texture(renderer,startup_heading_font.atlas));
                    replace_texture(dialogue_textures,851,create_image_texture(renderer,black_text));
                }
                for(int icon:{9,10,11,12,13,15}) {
                    const std::string path="fr-FR/_img_glb__"+std::to_string(icon)+".png";
                    if(archive.contains(path))
                        replace_texture(dialogue_textures,0x50000+icon,create_image_texture(renderer,decode_indexed_png(archive.read_entry(path))));
                }
                if(archive.contains("fr-FR/_img_etcui__0.png"))
                    replace_texture(combat_textures,0x3ff,create_image_texture(renderer,decode_indexed_png(archive.read_entry("fr-FR/_img_etcui__0.png"))));
                if(archive.contains("fr-FR/_img_etcui__1.png"))
                    replace_texture(combat_textures,1,create_image_texture(renderer,decode_indexed_png(archive.read_entry("fr-FR/_img_etcui__1.png"))));
                if(archive.contains("fr-FR/_img_etcui__4.png"))
                    replace_texture(dialogue_textures,0x51010,create_image_texture(renderer,decode_indexed_png(archive.read_entry("fr-FR/_img_etcui__4.png"))));
            } catch(const std::exception& error) {startup_log(std::string("language reload failed: ")+error.what());}
        };
        bool trigger_left_held = false, trigger_right_held = false;
        int stick_direction = 0; // left stick as a D-pad direction (1..4), 0 centred
        std::string world_notice;
        Uint32 world_notice_until{};
        bool enemies_live = false; // false right after a fresh decode_enemies() of the EVT
        const auto load_boss_assets = [&](int map_id) {
            const int file = map_id == 11 ? 1 : map_id == 13 ? 2 : map_id == 15 ? 3 : map_id == 82 ? 4 : 0;
            if (!file) return;
            static constexpr int first[5]{0, 32, 35, 39, 42}, last[5]{0, 32, 38, 41, 42};
            std::unordered_set<std::size_t> frames;
            for (int id = first[file]; id <= last[file]; ++id) {
                try {
                    auto animation = load_boss_animations(archive, file, id);
                    const auto count = [&](std::size_t type, std::size_t dir) {
                        return int(std::max<std::size_t>(1, animation.types[type][dir].size()));
                    };
                    combat::EnemyFrames counts;
                    counts.idle = count(0, 0); counts.walk = count(1, 0);
                    counts.death = count(2, 0); counts.attack = count(3, 0);
                    for (std::size_t d = 0; d < 4; ++d) counts.attack_by_direction[d] = count(3, d);
                    for (std::size_t set = 0; set < 3; ++set) counts.alt_attack[set] = count(3 + set, 0);
                    combat_world.set_enemy_frames(id, counts);
                    for (const auto& type : animation.types)
                        for (const auto& direction : type)
                            for (const auto& frame : direction)
                                for (const auto& layer : frame) frames.insert(layer.frame);
                    npc_assets.boss_animations[id] = std::move(animation);
                    if (archive.contains("boss/atef/" + std::to_string(id))) {
                        auto effect = load_eif(archive, "boss/atef/" + std::to_string(id));
                        for (const auto& frame : effect) for (const auto& layer : frame) frames.insert(layer.atlas_frame);
                        counts.effect = std::max(1, int(effect.size()));
                        combat_world.set_enemy_frames(id, counts);
                        npc_assets.effects[id] = std::move(effect);
                    }
                } catch (const std::exception& error) {
                    startup_log("boss sprite " + std::to_string(id) + " failed: " + error.what());
                }
            }
            const auto textures = load_sprite_textures(renderer, archive, "boss/" + std::to_string(file), frames);
            // The encounter shares one atlas, stored under its first stats id (boss_texture_base).
            for (const auto& texture : textures)
                npc_assets.enemy_textures.emplace((first[file] << 8) | int(texture.first), texture.second);
        };
        const auto rebuild_combat = [&](int map_id) {
            if (!enemies_live) enemy_spawn_records = enemies;
            enemies_live = true;
            std::vector<std::int8_t> overlay(event_grid.overlay.size());
            for (std::size_t i = 0; i < overlay.size(); ++i) overlay[i] = static_cast<std::int8_t>(event_grid.overlay[i]);
            combat_world.reset(map.width, map.height, std::move(overlay), &enemy_stats, SDL_GetTicks());
            combat_world.restart_regen(persistent_progress.level); // n.f -> ao.a
            { // ac.g registers visible NPCs at load, before ae.d places the enemies
                std::vector<std::pair<int, int>> npc_tiles;
                for (std::size_t i = 0; i < npcs.size(); ++i)
                    if (i >= event_grid.npc_visible.size() || event_grid.npc_visible[i]) npc_tiles.emplace_back(npcs[i].x, npcs[i].y);
                combat_world.set_blocked_tiles(std::move(npc_tiles));
            }
            for (const auto& record : enemy_spawn_records) {
                const int id = record.code >= 128 ? int(record.code) - 256 : record.code;
                if (record.resource_slot < 0 || id < 0 || std::size_t(id) >= enemy_stats.size()) continue;
                const auto found = npc_assets.enemy_animations.find(id);
                if (found != npc_assets.enemy_animations.end())
                    combat_world.set_enemy_frames(id, {int(std::max<std::size_t>(1, found->second.idle[0].size())),
                        int(std::max<std::size_t>(1, found->second.walk[0].size())),
                        int(std::max<std::size_t>(1, found->second.attack[0].size()))});
                if (const auto effect = npc_assets.effects.find(id); effect != npc_assets.effects.end()) {
                    combat::EnemyFrames counts{int(std::max<std::size_t>(1, found->second.idle[0].size())),
                        int(std::max<std::size_t>(1, found->second.walk[0].size())),
                        int(std::max<std::size_t>(1, found->second.attack[0].size()))};
                    counts.effect = std::max(1, int(effect->second.size()));
                    combat_world.set_enemy_frames(id, counts);
                }
                combat_world.queue_spawn(id, id, record.x, record.y, 0);
            }
            load_boss_assets(map_id);
            combat_world.spawn_bosses(map_id);
            enemies.clear();
        };
        // Live enemy tiles feed the existing hero/NPC collision code; frame_index = combat slot.
        const auto sync_live_enemies = [&]() {
            enemies.clear();
            const auto& live = combat_world.enemies();
            for (std::size_t i = 0; i < live.size(); ++i) {
                if (live[i].state == combat::EnemyState::dead) continue;
                MapEntity entity{};
                entity.x = static_cast<std::uint8_t>(live[i].tile_x);
                entity.y = static_cast<std::uint8_t>(live[i].tile_y);
                entity.code = static_cast<std::uint8_t>(live[i].sprite);
                entity.resource_slot = 0;
                entity.frame_index = int(i);
                entity.partial_x = live[i].partial_x;
                entity.partial_y = live[i].partial_y;
                enemies.push_back(entity);
                for (int extra = 1; extra < live[i].width; ++extra) { // second column of 2-wide enemies blocks too
                    MapEntity tail = entity; tail.x = static_cast<std::uint8_t>(entity.x + extra);
                    tail.resource_slot = -1; tail.frame_index = -1; enemies.push_back(tail);
                }
            }
        };
        const auto combat_view = [&]() {
            combat::HeroCombatView view;
            const auto hero = hero_input.state();
            view.x_tile = hero.x_tile; view.y_tile = hero.y_tile;
            view.partial_x = hero.partial_x; view.partial_y = hero.partial_y;
            view.direction = hero.direction;
            view.character_class = new_game.character_class;
            view.level = persistent_progress.level;
            view.hp = persistent_vitals.hp; view.max_hp = persistent_vitals.max_hp;
            view.sp = persistent_vitals.sp; view.max_sp = persistent_vitals.max_sp;
            view.agility = persistent_progress.attributes[2];
            view.base_will = persistent_progress.attributes[3];
            view.moving = hero.motion_mode == 2;
            view.max_combo = persistent_progress.max_combo;
            view.combat_map = !hero_assets.peaceful;
            for (std::size_t i = 0; i < 3; ++i)
                if (!hero_assets.magic[i].empty()) view.magic_frames[i] = int(hero_assets.magic[i].size());
            if (persistent_guardians.equipped < persistent_guardians.slots.size())
                if (const auto& active = persistent_guardians.slots[persistent_guardians.equipped]) {
                    view.guardian_id = active->id; view.guardian_level = active->level;
                    view.guardian_calm = active->calm; view.guardian_wild = active->wild;
                }
            // n.e(int): quest 29 grants the sword hero a second combo step.
            if (new_game.character_class == 6 && persistent_quests.size() > 3 && (persistent_quests[3] >> 5 & 1))
                view.max_combo = std::max(view.max_combo, 2);
            // ao.n: h = weapon a + e*5/2 + (power + m)*4/5; o adds to agility, armour e to hit.
            const auto gear_total = gear_totals(equipped_items, item_catalog);
            view.attack = combat::hero_base_attack(persistent_progress.attributes[0] + gear_total.bonus[0]) + gear_total.weapon;
            // ao.n: i = gear + (power + m)/5 + level/3; regeneration and the critical roll add n/p too.
            view.defense = combat::hero_base_defense(persistent_progress.attributes[0] + gear_total.bonus[0], persistent_progress.level) + gear_total.defense;
            view.agility += gear_total.bonus[2];
            view.hp_attribute = persistent_progress.attributes[1] + gear_total.bonus[1];
            view.will = persistent_progress.attributes[3] + gear_total.bonus[3];
            view.accessory_evasion = gear_total.armour_refine;
            view.weapon_refine = equipped_items[0] ? equipped_items[0]->refine : 0;
            for (std::size_t slot = 0; slot < equipped_items.size(); ++slot) if (equipped_items[slot]) {
                const auto& gear = *equipped_items[slot];
                if (item_catalog.empty()) continue;
                if (const auto* item = find_catalog_item(item_catalog, gear.item_type, gear.item_id))
                    if (const auto info = equipment_info(*item)) {
                        if (slot == 0) {
                            view.weapon_power = info->power;
                            if(const auto properties=weapon_properties(*item)) {
                                view.weapon_critical_chance=properties->critical_chance;
                                view.weapon_critical_bonus_tenths=properties->critical_bonus_tenths;
                                view.weapon_hit_option=item_option(gear,item);
                            }
                        }
                        else if(slot==1) view.shield_hit_option=item_option(gear,item);
                    }
            }
            return view;
        };
        try { item_catalog = load_item_catalog(archive); }
        catch (const std::exception& error) { startup_log(std::string("item catalog failed: ") + error.what()); }
        // ad.a(type,id,true,true): new gear rolls its bonuses with h.a (shared game Random).
        const auto roll_gear = [&](InventoryStack& stack) {
            if (const auto* item = find_catalog_item(item_catalog, stack.item_type, stack.item_id))
                roll_equipment(stack, *item, true, [&](int lo, int hi) { return combat_world.rand_range(lo, hi); });
        };
        std::vector<MixRecipe> mix_table;
        try { mix_table = parse_mix_table(archive.read_entry("itm/mixtbl")); }
        catch (const std::exception& error) { startup_log(std::string("mix table failed: ") + error.what()); }
        rebuild_combat(current_map_id);
        struct ShopRow { const ShopItem* item; std::size_t slot; };
        std::vector<ShopRow> shop_rows;
        bool shop_selling = false;
        bool shop_confirming = false;  // ab: quantity / price window
        bool shop_asking = false;      // af type 2: "Will you buy?" / "Will you sell?"
        std::string shop_question;
        int shop_quantity = 1;
        std::string shop_notice;
        int shop_tab = 0;
        std::size_t shop_selection = 0;
        std::string dialogue_text;
        int dialogue_scroll_lines = 0;
        WorldCamera world_camera;
        struct MotionSnapshot { SDL_Point hero{}, camera{}; std::vector<SDL_Point> npcs, enemies; WorldViewport view{}; int map{-1}; };
        MotionSnapshot motion_before;
        DialogueReveal dialogue_reveal;
        bool dialogue_active = false;
        bool item_message_box = false; // ah state 4 message, drawn as a small box
        int dialogue_portrait = 0;
        std::string event_status_text;
        bool dialogue_advance_requested = false;
        bool choice_active = false;
        bool choice_selected_yes = true;
        int ui_detail_page = 0;
        std::optional<EventScreen> service_screen;
        // aa / ax: blacksmith and refiner windows (service.hpp), refining pause timer.
        std::optional<ServiceSession> service;
        Uint32 service_busy_until = 0;
        ServiceView service_view;
        // bw: loaded sounds by id; music slots follow the title, map, events and the ending.
        std::array<std::shared_ptr<const MidiSong>,32> midi_sounds{};
        std::array<std::shared_ptr<const WavSound>,32> wav_sounds{};
        const auto load_sound=[&](int id) {
            if(id<0 || id>=32 || midi_sounds[std::size_t(id)] || wav_sounds[std::size_t(id)]) return;
            const std::string name=kSoundFiles[id];
            try {
                const auto bytes=archive.read_entry("snd/"+name);
                if(name.size()>4 && name.compare(name.size()-4,4,".wav")==0) wav_sounds[std::size_t(id)]=std::make_shared<WavSound>(parse_wav(bytes));
                else midi_sounds[std::size_t(id)]=std::make_shared<MidiSong>(parse_midi(bytes));
            } catch(const std::exception& error) {startup_log("sound "+name+" failed: "+error.what());}
        };
        const auto play_sound=[&](int id) { // bw.a(id,false): WAV effect, or a MIDI jingle once
#if HEROES_LORE_PSP
            psp_diag_stage("sfx-begin",id);
#endif
            load_sound(id);
            if(id<0 || id>=32) return;
            if(wav_sounds[std::size_t(id)]) audio.play_sound(wav_sounds[std::size_t(id)]);
            else if(midi_sounds[std::size_t(id)]) audio.play_music(AudioMixer::jingle,midi_sounds[std::size_t(id)],false);
#if HEROES_LORE_PSP
            psp_diag_stage("sfx-done",id);
#endif
        };
        const auto play_music=[&](AudioMixer::Slot slot,int id) { // bw.b(int) / bw.c(int): looped
            load_sound(id);
            if(id>=0 && id<32 && midi_sounds[std::size_t(id)]) audio.play_music(slot,midi_sounds[std::size_t(id)],true);
            else audio.stop_music(slot);
        };
        int current_music=-2;
        // as states 12/13: ending story and staff roll; afterwards n.c() starts Hardcore mode.
        std::optional<EndingSequence> ending;
        EndingView ending_view;
        const auto service_inventory = [&]() {
            return ServiceInventory{persistent_inventory, equipped_items, persistent_money, item_catalog};
        };
        const auto open_service = [&](EventScreen screen) {
            service.emplace();
            service->kind = screen == EventScreen::refiner ? ServiceKind::refiner : ServiceKind::blacksmith;
            service_busy_until = 0;
        };
        const auto service_input = [&](int direction, bool accept, bool cancel) {
            if (direction) service->navigate(direction);
            else if (accept) service->confirm(service_inventory(), mix_table,
                [&](int lo, int hi) { return combat_world.rand_range(lo, hi); });
            else if (cancel) service->back();
            if (service->done) {
                service.reset();
                screen_return_active = false;
                service_screen.reset();
                event_status_text.clear();
            }
        };
        std::optional<bool> choice_response;
        std::optional<std::size_t> last_auto_trigger_cell;
        if(startup_save) last_auto_trigger_cell=std::size_t(hero_input.state().y_tile)*map.width+hero_input.state().x_tile;
        // Port recipe journal: the recipe lines seen so far, one entry per result, in story order.
        const auto known_recipes = [&]() {
            std::vector<RecipeEntry> list;
            for (std::size_t slot = 0; slot < kRecipeTexts.size(); ++slot) {
                const auto index = std::size_t(kRecipeTexts[slot]);
                if (!recipe_known(persistent_recipes, int(slot)) || index >= language_bank.size()) continue;
                auto entry = parse_recipe_text(language_bank[index]);
                if (std::none_of(list.begin(), list.end(), [&](const RecipeEntry& e){return e.result == entry.result;}))
                    list.push_back(std::move(entry));
            }
            return list;
        };
        const auto shop_text = [&](const std::string& key) {
            // Keys are decimal indices into the active language bank.
            if (key.empty()) return std::string("[missing text]");
            std::size_t index = 0;
            for (const unsigned char c : key) {
                if (c < '0' || c > '9' || index >= language_bank.size())
                    return std::string("[") + key + "]";
                index = index * 10 + (c - '0');
            }
            return index < language_bank.size() ? format_game_text(language_bank[index])
                                               : std::string("[") + key + "]";
        };
        const auto item_stats_text = [&](const ShopItem& item) {
            const auto info = equipment_info(item);
            if (!info) return std::string{};
            return (item.type <= 2 ? tr("Base attack: ","Атака ") : tr("Base defense: ","Защита ")) +
                std::to_string(info->power);
        };
        const auto refresh_shop = [&]() {
            ui_detail_page=0;
            if (shop_selling && persistent_inventory.empty() && shop_notice.empty() && !shop_confirming) {
                // bb: once the last item is sold the list closes back to the shop with shop 21/22.
                shop_selling = false;
                shop_selection = 0;
                shop_notice = shop_ui_text.size()>22 ? shop_ui_text[21]+"\n"+shop_ui_text[22] : std::string();
            }
            shop_rows.clear();
            if (shop_selling) {
                for (std::size_t slot = 0; slot < persistent_inventory.size(); ++slot) {
                    const auto& stack = persistent_inventory[slot];
                    const auto found = std::find_if(item_catalog.begin(), item_catalog.end(), [&](const auto& item) {
                        return item.type == static_cast<std::uint8_t>(stack.item_type) &&
                               item.id == static_cast<std::uint8_t>(stack.item_id);
                    });
                    shop_rows.push_back({found == item_catalog.end() ? nullptr : &*found, slot});
                }
            } else {
                for (const auto& item : shop_catalog)
                    if (item.shop_category == shop_tab) shop_rows.push_back({&item, 0});
            }
            if (shop_rows.empty()) shop_selection = 0;
            else shop_selection = std::min(shop_selection, shop_rows.size() - 1);
            if (shop_confirming && !shop_rows.empty() && shop_rows[shop_selection].item) {
                const auto& item = *shop_rows[shop_selection].item;
                event_status_text = std::string(shop_selling ? "SELL " : "BUY ") + shop_text(item.name_key) + " x" +
                    std::to_string(shop_quantity) + "\nTotal: " +
                    std::to_string(shop_selling ? shop_sale_value(item, shop_quantity)
                                               : static_cast<std::int64_t>(item.price) * shop_quantity) +
                    "  Gold: " + std::to_string(persistent_money) +
                    "\nLeft/Right: quantity  A/Enter: confirm  B/Esc: cancel  X: text";
                dialogue_scroll_lines = 0;
                return;
            }
            event_status_text = (shop_selling ? "SELL" : "BUY " + std::to_string(shop_tab + 1) + "/6") +
                std::string("  Gold: ") +
                std::to_string(persistent_money) + "  Y/Tab: buy/sell  A: select  X: text  B: back\n";
            if (shop_rows.empty()) event_status_text += shop_selling ? "Inventory is empty." : "No items in this category.";
            else if (!shop_rows[shop_selection].item) event_status_text += "Item metadata unavailable; cannot sell this slot.";
            else {
                const auto& item = *shop_rows[shop_selection].item;
                event_status_text += std::to_string(shop_selection + 1) + "/" +
                    std::to_string(shop_rows.size()) + "  " + shop_text(item.name_key) +
                    (shop_selling ? "  Sell: " + std::to_string(shop_sale_value(item, 1)) +
                        "  Owned: " + std::to_string(persistent_inventory[shop_rows[shop_selection].slot].count)
                        : "  Price: " + std::to_string(item.price)) + "\n" +
                    (shop_selling && !can_sell_item_type(item.type) ? "Cannot sell this item. " : "") +
                    item_stats_text(item) + "\n" + shop_text(item.description_key);
            }
            if (!shop_notice.empty()) event_status_text = shop_notice + "\n" + event_status_text;
            dialogue_scroll_lines = 0;
        };
        const auto confirm_shop = [&]() {
            if (tab_focus) {tab_focus = false;refresh_shop();return;} // Modern: A leaves the category column
            if (shop_rows.empty() || !shop_rows[shop_selection].item) return;
            const auto& row = shop_rows[shop_selection];
            if (!shop_notice.empty()) {shop_notice.clear();refresh_shop();return;} // af type 1 acknowledged
            if (shop_selling && !can_sell_item_type(row.item->type)) {
                shop_notice = std::size_t(20)<shop_ui_text.size()?shop_ui_text[19]+"\n"+shop_ui_text[20]:
                    tr("This item cannot be sold.","Этот предмет нельзя продать.");
                refresh_shop();
                return;
            }
            const auto st=[&](int i){return std::size_t(i)<shop_ui_text.size()?shop_ui_text[std::size_t(i)]:std::string();};
            if (!shop_confirming) {
                shop_confirming = true;
                shop_quantity = 1;
                shop_notice.clear();
            } else if (!shop_asking) {
                // ab: A asks once more; equipment for another hero adds "You can't equip it." (shop 26).
                shop_asking = true;
                shop_question = shop_selling ? st(23) : st(7);
                if (!shop_selling) {
                    const int t=row.item->type,cls=new_game.character_class;
                    const bool foreign=(t==0 && cls!=6) || (t==2 && cls!=7) || (t==1 && cls!=8) || (t==3 && cls!=8);
                    if (foreign) shop_question = st(26) + "\n" + st(7);
                }
            } else {
                shop_confirming = false;
                shop_asking = false;
                if (shop_selling) {
                    const auto result = sell_shop_item(*row.item, row.slot, shop_quantity,
                        persistent_inventory, persistent_money);
                    shop_notice = result == ShopSaleResult::sold ? shop_text(row.item->name_key) + "\n" + st(24)
                                                                 : st(19) + "\n" + st(20);
                    if (result == ShopSaleResult::sold) shop_selection = 0; // bb is rebuilt after a sale
                    startup_log("sale result=" + std::to_string(static_cast<int>(result)) +
                        ", slot=" + std::to_string(row.slot));
                } else {
                    const auto result = purchase_shop_item(*row.item, shop_quantity,
                        persistent_inventory, persistent_money);
                    switch (result) {
                        case ShopPurchaseResult::purchased: shop_notice = st(11) + "\n" + st(12); break;
                        case ShopPurchaseResult::insufficient_money: shop_notice = st(8); break;
                        case ShopPurchaseResult::inventory_full: shop_notice = st(9) + "\n" + st(10); break;
                        default: shop_notice = tr("Invalid purchase.","Покупка невозможна."); break;
                    }
                    startup_log("purchase result=" + std::to_string(static_cast<int>(result)));
                }
                startup_log("shop gold=" + std::to_string(persistent_money) +
                    ", slots=" + std::to_string(persistent_inventory.size()));
            }
            refresh_shop();
        };
        const auto page_shop_text = [&]() { ++ui_detail_page; };
        const auto navigate_shop = [&](int direction, bool shoulder = false) {
            if (!shop_notice.empty() || shop_asking) return;
            if (shop_confirming) {
                if (!shop_rows.empty() && shop_rows[shop_selection].item) {
                    const auto& row = shop_rows[shop_selection];
                    shop_quantity = step_shop_quantity(shop_quantity, direction, shop_selling,
                        is_stackable_item(row.item->type), shop_selling ? persistent_inventory[row.slot].count : 0);
                }
                refresh_shop();
                return;
            }
            // Modern: the categories are a column; Left enters it, Up/Down pick there, Right returns.
            if (modern_layout && !shoulder && !shop_selling) {
                if (direction == 3 || direction == 4) tab_focus = direction == 3;
                else if (tab_focus) {shop_tab = (shop_tab + (direction == 1 ? 5 : 1)) % 6;shop_selection = 0;}
                if (direction >= 3 || tab_focus) {refresh_shop();return;}
            }
            if (!shop_selling && (direction == 3 || direction == 4)) {
                shop_tab = (shop_tab + (direction == 3 ? 5 : 1)) % 6;
                shop_selection = 0;
            } else if (!shop_rows.empty()) {
                if (direction == 1) shop_selection = (shop_selection + shop_rows.size() - 1) % shop_rows.size();
                if (direction == 2) shop_selection = (shop_selection + 1) % shop_rows.size();
            }
            refresh_shop();
        };
        const auto toggle_shop_mode = [&]() {
            tab_focus = false;
            if (shop_confirming || shop_asking || !shop_notice.empty()) return;
            if (!shop_selling && persistent_inventory.empty()) { // v: "No items to sell." (shop 16/17)
                shop_notice = shop_ui_text.size()>17 ? shop_ui_text[16]+"\n"+shop_ui_text[17] : "";
                return;
            }
            shop_selling = !shop_selling;
            shop_selection = 0;
            shop_notice.clear();
            refresh_shop();
        };
        const auto refresh_menu = [&]() {
            ui_detail_page=0;
            dialogue_scroll_lines = 0;
            if (menu_page != 1) {
                const auto journal=build_quest_journal(new_game.character_class,quest_text,persistent_quests);
                quest_selection=journal.empty()?0:std::min(quest_selection,journal.size()-1);
                event_status_text = "MENU";
            } else if (menu_page == 1) {
                if (persistent_inventory.empty()) bag_selection = 0;
                else bag_selection = std::min(bag_selection, persistent_inventory.size()-1);
                if (equipment_confirming && !persistent_inventory.empty()) {
                    const auto& gear = persistent_inventory[bag_selection];
                    const auto* item = find_catalog_item(item_catalog, gear.item_type, gear.item_id);
                    event_status_text = "EQUIP " + (item ? shop_text(item->name_key) : "Unknown") +
                        "?\nA/Enter: confirm  B/Esc: cancel\n";
                    const int dest = equipment_slot(static_cast<std::uint8_t>(gear.item_type));
                    if (dest >= 0 && equipped_items[dest]) {
                        const auto& old = *equipped_items[dest];
                        const auto* previous = find_catalog_item(item_catalog, old.item_type, old.item_id);
                        event_status_text += "Replace: " + (previous ? shop_text(previous->name_key) : "Unknown");
                    }
                    return;
                }
                event_status_text = "BAG  Gold: " + std::to_string(persistent_money) +
                    "  Slots: " + std::to_string(persistent_inventory.size()) + "/30\n"
                    "Up/Down: item  A: equip  X: text  B: back\n";
                if (persistent_inventory.empty()) event_status_text += "Empty.\n";
                else {
                    const auto& stack = persistent_inventory[bag_selection];
                    const auto* item = find_catalog_item(item_catalog, stack.item_type, stack.item_id);
                    event_status_text += std::to_string(bag_selection+1) + "/" +
                        std::to_string(persistent_inventory.size()) + " " +
                        (item ? shop_text(item->name_key) : "Unknown item") +
                        " x" + std::to_string(stack.count) + "\n";
                    if (item) event_status_text += item_stats_text(*item) +
                        (stack.identified ? "" : " Unidentified") + "\n" +
                        shop_text(item->description_key) + "\n";
                }
                event_status_text += "EQUIPPED\n";
                const char* labels[] = {"Weapon", "Shield", "Armor", "Head", "Accessory"};
                for (std::size_t slot = 0; slot < equipped_items.size(); ++slot) {
                    if (!equipped_items[slot]) continue;
                    const auto& gear = *equipped_items[slot];
                    const auto* item = find_catalog_item(item_catalog, gear.item_type, gear.item_id);
                    event_status_text += std::string(labels[slot]) + ": " +
                        (item ? shop_text(item->name_key) : "Unknown") + "\n";
                }
                if (!equipment_notice.empty()) event_status_text = equipment_notice + "\n" + event_status_text;
            }
        };
        const auto close_menu = [&]() {
            attribute_editing=false;pending_attributes.fill(0);save_confirming=false;
            attribute_confirming=false;guardian_choice=-1;skill_screen=false;item_action=0;
            menu_active = false;
            screen_return_active = false;
            event_status_text.clear();
            dialogue_scroll_lines = 0;
        };
        const auto open_menu = [&]() {
            tab_focus = modern_layout; // Modern: the menu opens with the cursor in the tab column
            if (event_runtime || screen_return_active) return;
            if (hero_input.state().motion_mode != 1) {
                menu_requested = true;
                interaction_requested = false;
                hero_input.release();
                return;
            }
            menu_requested = interaction_requested = false;
            menu_active = screen_return_active = true;
            menu_page = menu_selection = 0;
            equipment_confirming = false;unequip_confirming = false;
            quest_reading = false;
            equipment_notice.clear();
            try {
                if (item_catalog.empty()) item_catalog = load_item_catalog(archive);
            } catch (const std::exception& error) { startup_log(error.what()); }
            refresh_menu();
        };
        int deferred_attack = 0; // 1 normal, 2 skill: requested mid-step, started once aligned
        // ao.n: gear bonuses, max HP/SP and the clamp of the current values, always derived from
        // attributes + equipment. Runs after loading (ao.a([B) ends with n()) and before saving, so a
        // save never keeps the maxima of gear destroyed earlier in the same frame.
        const auto sync_derived_stats=[&]() {
            if(item_catalog.empty()) try {item_catalog=load_item_catalog(archive);} catch(const std::exception& error) {startup_log(error.what());return;}
            persistent_progress.gear_bonus=gear_totals(equipped_items,item_catalog).bonus;
            recalculate_vitals(persistent_progress,persistent_vitals);
            persistent_vitals.hp=std::min(persistent_vitals.hp,persistent_vitals.max_hp);
            persistent_vitals.sp=std::min(persistent_vitals.sp,persistent_vitals.max_sp);
        };
        const auto capture_save = [&]() {
            const auto hero=hero_input.state();
            if(event_runtime || hero.motion_mode!=1 || hero.partial_x || hero.partial_y)
                throw std::runtime_error(tr("Finish the current action before saving.","Сначала завершите текущее действие."));
            if(!map_allows_save(current_map_id)) throw std::runtime_error(tr("You cannot save during the boss stage.","Во время боя с боссом сохраняться нельзя."));
            sync_derived_stats();
            SaveGame saved;
            saved.character_class=new_game.character_class;saved.map_id=current_map_id;saved.zone=std::stoi(current_zone);
            saved.x=hero.x_tile;saved.y=hero.y_tile;saved.direction=hero.direction;saved.quick_item_type=quick_item_type;
            saved.switches=persistent_switches;saved.quests=persistent_quests;saved.recipes=persistent_recipes;saved.inventory=persistent_inventory;
            saved.money=persistent_money;saved.equipment=equipped_items;saved.vitals=persistent_vitals;
            saved.progress=persistent_progress;saved.guardians=persistent_guardians;
            saved.new_game_plus=persistent_new_game_plus;saved.clear_progress=persistent_clear_progress;return saved;
        };
        std::function<void(int)> boot_progress;
        const auto restore_save = [&](const SaveGame& saved) {
            shown_level=0;
            validate_saved_resources(saved);
            if(boot_progress) boot_progress(10);
            auto next_profile=make_new_game_profile(saved.character_class);
            auto next_quest_text=parse_tdf_text(archive.read_entry("sgui/q"+std::to_string(saved.character_class)+".tdf"),language_bank);
            auto next_map=load_map(archive,saved.map_id);
            auto next_event=load_event_grid(archive,std::to_string(saved.zone),saved.map_id,next_map);
            if(const auto group=select_setup_group(next_event,saved.switches)) apply_setup_group(next_map,next_event,group->second);
            auto next_objects=decode_objects(next_event),next_npcs=decode_npcs(next_event),next_enemies=decode_enemies(next_event);
            auto next_motion=make_npc_motion_states(next_event);
            HeroInput next_hero(next_map,next_event,next_npcs,next_enemies);
            next_hero.place_on_tile(saved.x,saved.y);next_hero.apply_script_command(6,std::uint8_t(saved.direction));
            SpriteTextures next_terrain,next_objects_tex;NpcVisualAssets next_npc_assets;HeroVisualAssets next_hero_assets;
            try {
                next_terrain=load_terrain_textures(renderer,archive,next_map);
                next_objects_tex=load_object_textures(renderer,archive,next_map,next_event,next_objects);
                if(boot_progress) boot_progress(40);
                next_npc_assets=load_npc_textures(renderer,archive,next_npcs);
                load_enemy_textures(renderer,archive,next_enemies,next_npc_assets);
                if(boot_progress) boot_progress(70);
                next_hero_assets=load_hero_textures(renderer,archive,saved.character_class,saved.equipment);
            } catch(...) {
                destroy_textures(next_terrain);destroy_textures(next_objects_tex);
                destroy_textures(next_npc_assets.textures);destroy_textures(next_npc_assets.enemy_textures);
                destroy_textures(next_hero_assets.textures);throw;
            }
            destroy_textures(terrain_textures);destroy_textures(object_textures);
            destroy_textures(npc_assets.textures);destroy_textures(npc_assets.enemy_textures);destroy_textures(hero_assets.textures);
            terrain_textures=std::move(next_terrain);object_textures=std::move(next_objects_tex);
            npc_assets=std::move(next_npc_assets);hero_assets=std::move(next_hero_assets);
            map=std::move(next_map);event_grid=std::move(next_event);objects=std::move(next_objects);
            npcs=std::move(next_npcs);enemies=std::move(next_enemies);npc_motion=std::move(next_motion);hero_input=std::move(next_hero);
            deferred_attack=0;game_over_until=0;
            combat_world.reset_hero_state();
            enemies_live=false;rebuild_combat(saved.map_id);
            new_game=std::move(next_profile);quest_text=std::move(next_quest_text);
            world_camera.reset();
            current_map_id=saved.map_id;current_zone=std::to_string(saved.zone);
            persistent_switches=saved.switches;persistent_quests=saved.quests;persistent_recipes=saved.recipes;persistent_inventory=saved.inventory;
            persistent_money=saved.money;equipped_items=saved.equipment;persistent_vitals=saved.vitals;
            persistent_progress=saved.progress;persistent_guardians=saved.guardians;quick_item_type=saved.quick_item_type;
            sync_derived_stats();
            combat_world.restart_regen(persistent_progress.level); // the map was rebuilt before the level arrived
            if(saved.new_game_plus!=persistent_new_game_plus) {
                try {enemy_stats=combat::parse_enemy_table(archive.read_entry(enemy_table_path(saved.new_game_plus)));}
                catch(const std::exception& error) {startup_log(std::string("enemy table load failed: ")+error.what());}
            }
            persistent_new_game_plus=saved.new_game_plus;persistent_clear_progress=saved.clear_progress;
            event_visuals=make_event_visual_state(event_grid);location_started=SDL_GetTicks();
            last_auto_trigger_cell=std::size_t(saved.y)*map.width+saved.x;
            dialogue_active=choice_active=false;dialogue_text.clear();dialogue_scroll_lines=0;
            interaction_requested=menu_requested=false;hero_input.release();close_menu();
            if(boot_progress) boot_progress(100);
        };
        const auto begin_new_game = [&](int character_class,const std::array<bool,3>& choices) {
            const auto profile=make_new_game_profile(character_class);
            SaveGame fresh;
            fresh.character_class=profile.character_class;fresh.map_id=profile.map_id;
            fresh.zone=profile.character_class;fresh.x=profile.x;fresh.y=profile.y;fresh.direction=profile.direction;
            fresh.switches=profile.switches;fresh.quests=profile.quests;fresh.equipment=profile.equipment;
            fresh.progress.attributes=profile.base_attributes;
            recalculate_vitals(fresh.progress,fresh.vitals);fresh.vitals.hp=fresh.vitals.max_hp;fresh.vitals.sp=fresh.vitals.max_sp;
            fresh.guardians=make_guardian_roster(choices);
            restore_save(fresh);new_game.guardian_choices=choices;last_auto_trigger_cell.reset();
        };
        TitleMenu title_menu(use_new_game_spawn,!title_saves.empty(),startup_notice);
        if(title_menu.active) title_menu.page=TitlePage::splash; // bg.b(): the logo splash comes first
        title_menu.saves=title_saves;
        const std::string settings_path=(save_dir/"settings.hlc").string();
        bool settings_enabled=true;
#if !HEROES_LORE_CONSOLE
        // Visual fixtures neither consume nor overwrite personal preferences.
        settings_enabled=std::getenv("HEROES_LORE_SNAPSHOT_SCREEN")==nullptr;
#endif
        if(settings_enabled) {
            const auto saved=read_port_settings(settings_path);
            title_menu.game_speed=saved.game_speed;title_menu.instant_speech=saved.instant_speech;
            title_menu.smooth_camera=saved.smooth_camera;title_menu.heroes_unlocked=saved.heroes_unlocked;title_menu.sound=saved.sound;title_menu.soundfont=saved.soundfont;title_menu.smooth_motion=saved.smooth_motion;title_menu.wide_ui=saved.wide_ui;title_menu.autosave=saved.autosave;title_menu.modern_ui=saved.modern_ui;title_menu.rumble=saved.rumble;
        }
        title_menu.language=session_language;
        // ah GAMEOVER -> as.g(): /sgui/ed<hero> story and /sgui/edsr staff roll over img/end.
        const auto start_ending=[&]() {
            const int hero=new_game.character_class;
            std::vector<std::string> story,staff;
            try {
                story=parse_tdf_text(archive.read_entry("sgui/ed"+std::to_string(hero)+".tdf"),language_bank);
                staff=parse_tdf_text(archive.read_entry("sgui/edsr.tdf"),language_bank);
                auto pictures=load_sprite_textures(renderer,archive,"img/end",{std::size_t(hero-6)});
                if(!pictures.empty()) replace_texture(dialogue_textures,0x52000,pictures.begin()->second);
            } catch(const std::exception& error) {startup_log(std::string("ending load failed: ")+error.what());}
            ending.emplace();ending->start(std::move(story),std::move(staff));
            audio.stop_all();current_music=-2; // as.g(): bw.f(), then 23
            hero_input.release();close_menu();
        };
        // n.c(): bank this run's progress, unlock Reah/Kail, then Hardcore mode restarts the
        // story at the hero's start with level, gear and guardians kept and quest items removed.
        const auto finish_ending=[&]() {
            ending.reset();
            const auto profile=make_new_game_profile(new_game.character_class);
            SaveGame next;
            next.character_class=new_game.character_class;next.quick_item_type=quick_item_type;
            next.switches=persistent_switches;next.quests=persistent_quests;next.recipes=persistent_recipes;
            next.inventory=persistent_inventory;next.money=persistent_money;next.equipment=equipped_items;
            next.vitals=persistent_vitals;next.progress=persistent_progress;next.guardians=persistent_guardians;
            next.new_game_plus=persistent_new_game_plus;
            next.clear_progress=std::min(100,persistent_clear_progress+run_progress_percent(next));
            next.new_game_plus=1;
            next.map_id=profile.map_id;next.zone=profile.character_class;next.x=profile.x;next.y=profile.y;
            next.direction=profile.direction;next.switches=profile.switches;next.quests=profile.quests;
            next.inventory.erase(std::remove_if(next.inventory.begin(),next.inventory.end(),
                [](const InventoryStack& s){return s.item_type>=18 && s.item_type<=21;}),next.inventory.end()); // g.c(): ad.b()
            next.vitals.hp=next.vitals.max_hp;next.vitals.sp=next.vitals.max_sp;
            title_menu.heroes_unlocked=true;
            if(settings_enabled)
                try {write_port_settings(settings_path,{title_menu.game_speed,title_menu.instant_speech,title_menu.smooth_camera,title_menu.language,true,title_menu.sound,title_menu.soundfont,title_menu.smooth_motion,title_menu.wide_ui,title_menu.autosave,title_menu.modern_ui,title_menu.rumble});}
                catch(const std::exception& error) {startup_log(std::string("settings write failed: ")+error.what());}
            try {restore_save(next);last_auto_trigger_cell.reset();}
            catch(const std::exception& error) {startup_log(std::string("Hardcore start failed: ")+error.what());}
        };
        const auto dialogue_page_range=[&]() {
            int w=0,h=0;SDL_GetRendererOutputSize(renderer,&w,&h);
            const auto layout=shown_dialogue_layout(w,h,dialogue_font.height,dialogue_portrait);
            const auto glyphs=layout_bitmap_text(dialogue_font,dialogue_text,0,0,layout.inner.w,layout.scale);
            std::size_t start=0,end=0;
            for(const auto& glyph:glyphs) {
                const int line=glyph.y/(int(dialogue_font.height)*layout.scale);
                if(line<dialogue_scroll_lines) start=std::max(start,glyph.text_end);
                if(line<dialogue_scroll_lines+layout.visible_lines) end=std::max(end,glyph.text_end);
            }
            return std::make_pair(start,end);
        };
        const auto advance_dialogue=[&]() {
            const auto range=dialogue_page_range();
            if(!title_menu.instant_speech && !dialogue_reveal.complete(range.second)) {
                dialogue_reveal.end=range.second;return;
            }
            int w=0,h=0;SDL_GetRendererOutputSize(renderer,&w,&h);
            const int page_lines=shown_dialogue_layout(w,h,dialogue_font.height,dialogue_portrait).visible_lines;
            if(dialogue_line_count(dialogue_font,dialogue_text,w,h,dialogue_portrait)>dialogue_scroll_lines+page_lines) {
                dialogue_scroll_lines+=page_lines;dialogue_reveal.reset();
            } else dialogue_advance_requested=true;
        };
        int camera_map=-1,camera_width=0,camera_height=0;std::string camera_zone;
        Uint32 camera_tick=0;

        const auto confirm_menu = [&]() {
            // af type 1: a message stays until it is acknowledged.
            if(!equipment_notice.empty()) {equipment_notice.clear();refresh_menu();return;}
            if(tab_focus) {tab_focus=false;refresh_menu();return;} // Modern: A leaves the tab column
            if(menu_page==5) {
                // d: Save / Help / Options / Quit; Help and Options open the title pages over the game.
                if(save_selection==0) {
                    const auto gm=[&](int i,const char* fallback){return std::size_t(i)<menu_text.size()?menu_text[std::size_t(i)]:std::string(fallback);};
                    try {write_save(save_path_for(new_game.character_class),capture_save());save_available=true;equipment_notice=gm(46,"Game saved");}
                    catch(const std::exception& error) {startup_log(error.what());equipment_notice=gm(47,"Failed.");}
                    refresh_menu();return;
                }
                if(save_selection==1 || save_selection==2) {
                    title_menu.active=true;title_menu.in_game=true;title_menu.message.clear();
                    title_menu.page=save_selection==1?TitlePage::help:TitlePage::options;title_menu.selection=0;
                    return;
                }
                if(!save_confirming) {save_confirming=true;equipment_notice.clear();return;}
                save_confirming=false;
                try {
                    if(save_selection==0) {
                        write_save(save_path_for(new_game.character_class),capture_save());save_available=true;equipment_notice="Game saved.";refresh_menu();
                    } else {
                        close_menu();title_menu.open_menu();
                        scan_saves();title_menu.saves=title_saves;
                        title_menu.continue_available=!title_saves.empty();
                    }
                } catch(const std::exception& error) {equipment_notice=error.what();refresh_menu();}
                return;
            }
            if(attribute_editing) {
                // bi: with points placed, gm33 asks for confirmation; otherwise gm34/35 explain the keys.
                const bool placed=pending_attributes!=std::array<int,4>{};
                if(!placed) equipment_notice=menu_text.size()>35?menu_text[34]+"\n"+menu_text[35]:"";
                else if(!attribute_confirming) attribute_confirming=true;
                else {
                    attribute_confirming=false;
                    if(apply_attribute_points(persistent_progress,persistent_vitals,pending_attributes)) {
                        attribute_editing=false;pending_attributes.fill(0);
                    }
                }
                refresh_menu();return;
            }
            // Port: A on a worn piece takes it off into the bag (bz itself only swaps through the bag list).
            // The weapon stays: the original never leaves the hero unarmed.
            if(menu_page==2) {
                const std::size_t slot=equipment_selection;
                if(slot>=equipped_items.size() || !equipped_items[slot]) return;
                if(!unequip_confirming) {
                    if(slot==0) equipment_notice=tr("The weapon can only be swapped.","Оружие можно только заменить.");
                    else if(persistent_inventory.size()>=inventory_slot_limit) equipment_notice=bank_or(3938,"Inventory is full.");
                    else unequip_confirming=true;
                    refresh_menu();return;
                }
                unequip_confirming=false;
                auto candidate_equipment=equipped_items;candidate_equipment[slot].reset();
                try {
                    auto next_assets=load_hero_textures(renderer,archive,new_game.character_class,candidate_equipment,hero_assets.element);
                    destroy_textures(hero_assets.textures);hero_assets=std::move(next_assets);
                    persistent_inventory.push_back(*equipped_items[slot]);
                    equipped_items=std::move(candidate_equipment);
                } catch(const std::exception& error) {
                    equipment_notice="Equipment load failed; nothing changed.";startup_log(error.what());
                }
                refresh_menu();return;
            }
            if(menu_page==0 && menu_selection==3) {
                if(persistent_progress.attribute_points>0) {
                    pending_attributes.fill(0);attribute_selection=0;attribute_editing=true;
                } else equipment_notice=menu_text.size()>1?menu_text[0]:"";
                refresh_menu();return;
            }
            if(menu_page==3) {
                auto& slot=persistent_guardians.slots[guardian_selection];
                if(skill_screen && slot) {
                    // bn: a skill already on a key is "Already in use"; p.h gates skills by level.
                    static constexpr int skill_level[]{1,9,20,30};
                    const auto text=[&](int i){return std::size_t(i)<menu_text.size()?menu_text[std::size_t(i)]:std::string();};
                    if(skill_selection==slot->calm || skill_selection==slot->wild) equipment_notice=text(61);
                    else if(skill_level[skill_selection]>slot->level) equipment_notice=text(62)+"\n"+text(63);
                    else (skill_key_calm?slot->calm:slot->wild)=skill_selection;
                } else if(guardian_choice>=0 && slot) {
                    if(guardian_choice==0 && combat_world.guardian().state!=0) {
                        // ao.a(p): a summon in progress refuses the swap; bm shows menu 27..29.
                        const auto mt=[&](int i){return std::size_t(i)<menu_text.size()?menu_text[std::size_t(i)]:std::string();};
                        equipment_notice=mt(27)+"\n"+mt(28)+"\n"+mt(29);
                    } else if(guardian_choice==0) {
                        persistent_guardians.equipped=guardian_selection;
                        equipment_notice=(language_bank.size()>3933?language_bank[3933]:std::string())+
                            (std::size_t(slot->id)<guardian_text.size()?guardian_text[std::size_t(slot->id)]:"");
                    } else {skill_screen=true;skill_key_calm=true;skill_selection=0;}
                    guardian_choice=-1;
                } else if(slot) guardian_choice=0;
                refresh_menu();return;
            }
            if(menu_page==4 && recipes_view) return; // port: recipes have no reading view
            if(menu_page==4) {
                const auto journal=build_quest_journal(new_game.character_class,quest_text,persistent_quests);
                if(!journal.empty()) quest_reading=!quest_reading;
                refresh_menu();return;
            }
            if (menu_page == 1) {
                if (persistent_inventory.empty()) return;
                const auto gm=[&](int i){return std::size_t(i)<menu_text.size()?menu_text[std::size_t(i)]:std::string();};
                const auto& stack=persistent_inventory[bag_selection];
                const int type=stack.item_type;
                if(item_action==0) {
                    // ay.a(int,int): pick the af dialog for the selected item.
                    item_choice=0;item_prompt.clear();
                    if(equipment_slot(std::uint8_t(type))>=0) {
                        const int cls=new_game.character_class;
                        const bool own_weapon=(cls==6 && type==0) || (cls==7 && type==2) || (cls==8 && type==1);
                        if(!stack.identified) {item_action=6;item_prompt=gm(12);}
                        else if(type<=2 && !own_weapon) {item_action=6;item_prompt=gm(type==0?8:type==2?9:50)+"\n"+gm(12);}
                        else if(type==3 && cls!=8) {item_action=6;item_prompt=gm(12);}
                        else item_action=4;
                    } else if(is_quick_item(type)) item_action=5;
                    else if(type>=18 && type<=21) equipment_notice=gm(14);
                    else {item_action=6;item_prompt=gm(12);}
                    refresh_menu();return;
                }
                const int action=item_action;item_action=0;
                if((action==4 || action==5) && item_choice==1) {item_action=6;item_prompt=gm(12);refresh_menu();return;}
                if(action==6) {
                    // g.a(slot, 1): trash one unit.
                    if(--persistent_inventory[bag_selection].count<=0) {
                        persistent_inventory.erase(persistent_inventory.begin()+std::ptrdiff_t(bag_selection));
                        if(bag_selection>0 && bag_selection>=persistent_inventory.size()) --bag_selection;
                    }
                    refresh_menu();return;
                }
                if(action==5) {
                    bool antidote=false;
                    const auto result=use_consumable(persistent_inventory,bag_selection,persistent_vitals,&antidote);
                    if(result==ItemUseResult::used) {
                        if(antidote) combat_world.cure_poison();
                        quick_item_type=type;
                    }
                    if(!persistent_inventory.empty()) bag_selection=std::min(bag_selection,persistent_inventory.size()-1);
                    refresh_menu();return;
                }
                auto candidate_bag = persistent_inventory;
                auto candidate_equipment = equipped_items;
                const auto result = equip_bag_slot(new_game.character_class, bag_selection,
                    item_catalog, candidate_bag, candidate_equipment);
                if (result == EquipResult::equipped) {
                    try {
                        // Stage both the new assets and inventory before replacing live state.
                        auto next_assets = load_hero_textures(renderer, archive,
                            new_game.character_class, candidate_equipment, hero_assets.element);
                        destroy_textures(hero_assets.textures);
                        hero_assets = std::move(next_assets);
                        persistent_inventory = std::move(candidate_bag);
                        equipped_items = std::move(candidate_equipment);
                        if(!persistent_inventory.empty()) bag_selection=std::min(bag_selection,persistent_inventory.size()-1);
                        startup_log("equipment changed from bag slot=" + std::to_string(bag_selection));
                    } catch (const std::exception& error) {
                        equipment_notice = "Equipment load failed; inventory unchanged.";
                        startup_log(error.what());
                    }
                }
                refresh_menu();
                return;
            }
            if (menu_page == 0) menu_selection = (menu_selection + 1) % 4;
            refresh_menu();
        };
        const auto back_menu = [&]() {
            // af type 1: a message stays until it is acknowledged.
            if(!equipment_notice.empty()) {equipment_notice.clear();refresh_menu();return;}
            if(save_confirming) {save_confirming=false;return;}
            if(attribute_confirming) {attribute_confirming=false;refresh_menu();return;}
            if(guardian_choice>=0) {guardian_choice=-1;refresh_menu();return;}
            if(item_action) {item_action=0;refresh_menu();return;}
            if(skill_screen) {skill_screen=false;equipment_notice.clear();refresh_menu();return;}
            if(attribute_editing) {attribute_editing=false;pending_attributes.fill(0);refresh_menu();return;}
            if(quest_reading) {quest_reading=false;refresh_menu();return;}
            if (equipment_confirming) { equipment_confirming = false; refresh_menu(); return; }
            if (unequip_confirming) { unequip_confirming = false; refresh_menu(); return; }
            equipment_notice.clear();
            close_menu();
        };
        // Tabs may change only from the plain tab view (no sub-screen, prompt or editor open).
        const auto menu_tabs_free = [&]() {
            return equipment_notice.empty() && !attribute_confirming && guardian_choice<0 && !item_action &&
                   !skill_screen && !attribute_editing && !equipment_confirming && !unequip_confirming && !quest_reading && !save_confirming;
        };
        const auto navigate_menu = [&](int direction, bool shoulder = false) {
            if(!equipment_notice.empty()) return;
            if(attribute_confirming) return;
            if(guardian_choice>=0) {if(direction==1 || direction==2) guardian_choice^=1;refresh_menu();return;}
            if(item_action) {if((item_action==4 || item_action==5) && (direction==1 || direction==2)) item_choice^=1;refresh_menu();return;}
            if(skill_screen) {
                if(direction==3 || direction==4) skill_key_calm=!skill_key_calm;
                else if(direction==1 || direction==2) skill_selection=(skill_selection+(direction==1?2:1))%3;
                equipment_notice.clear();refresh_menu();return;
            }
            if(attribute_editing) {
                if(direction==1 || direction==2) attribute_selection=(attribute_selection+(direction==1?3:1))%4;
                if(direction==3 && pending_attributes[attribute_selection]>0) --pending_attributes[attribute_selection];
                if(direction==4 && attribute_points_remaining(persistent_progress,pending_attributes)>0)
                    ++pending_attributes[attribute_selection];
                return;
            }
            if (equipment_confirming || unequip_confirming || quest_reading || save_confirming) return;
            // Modern: the tabs are a column; Left enters it, Up/Down pick the tab, Right returns to the list.
            if (modern_layout && !shoulder) {
                if (direction == 3 || direction == 4) tab_focus = direction == 3;
                else if (tab_focus) {menu_page=(menu_page+(direction==1?5:1))%6;equipment_notice.clear();}
                if (direction >= 3 || tab_focus) {refresh_menu();return;}
            }
            if (direction == 3 || direction == 4) {
                menu_page=(menu_page+(direction==3?5:1))%6;
                equipment_notice.clear(); refresh_menu(); return;
            }
            if (direction != 1 && direction != 2) return;
            if (menu_page == 1 && !persistent_inventory.empty())
                bag_selection=move_grid_selection(bag_selection,persistent_inventory.size(),direction,1);
            else if (menu_page == 5) save_selection=(save_selection+(direction==1?3:1))%4;
            else if (menu_page == 3)
                guardian_selection=(guardian_selection+(direction==1?4:1))%5;
            else if (menu_page == 2)
                equipment_selection=(equipment_selection+(direction==1?4:1))%5;
            else if (menu_page == 4 && recipes_view)
                recipe_selection=move_grid_selection(recipe_selection,known_recipes().size(),direction,1);
            else if (menu_page == 4) {
                const auto journal=build_quest_journal(new_game.character_class,quest_text,persistent_quests);
                quest_selection=move_grid_selection(quest_selection,journal.size(),direction,1);
            } else if (menu_page == 0)
                menu_selection=(menu_selection+(direction==1?3:1))%4;
            equipment_notice.clear(); refresh_menu();
        };
        const auto try_start_event = [&](int x, int y, EventTriggerType type) {
            if (event_runtime || screen_return_active || hero_input.state().motion_mode != 1) return;
            if (x < 0 || y < 0 || x >= map.width || y >= map.height) return;
            const auto raw = event_grid.overlay[static_cast<std::size_t>(y) * map.width + x];
            const int signed_id = raw >= 128 ? int(raw) - 256 : raw;
            const int trigger_id = type == EventTriggerType::actor_contact
                ? signed_id : std::abs(signed_id);
            if (trigger_id < 1 || trigger_id > 127) return;
            const auto match = find_event_trigger(event_grid, trigger_id,
                type, persistent_switches,
                persistent_quests, [&](std::uint8_t a, std::uint8_t b, std::uint8_t count) {
                    return has_inventory_requirement(persistent_inventory, a, b, count);
                });
            if (!match || match->action_list >= event_grid.actions.size()) {
                startup_log("no event match: type=" + std::to_string(static_cast<int>(type)) +
                    ", trigger=" + std::to_string(trigger_id) +
                    ", tile=" + std::to_string(x) + "," + std::to_string(y) +
                    ", switch0=" + std::to_string(persistent_switches[0] & 1u));
                return;
            }
            active_action_storage = event_grid.actions[match->action_list];
            active_actions = &active_action_storage;
            event_runtime = std::make_unique<EventRuntime>(
                map, event_grid, persistent_switches, persistent_quests,
                persistent_inventory, persistent_money, persistent_vitals, persistent_guardians, persistent_progress);
            event_runtime->item_created = [&](InventoryStack& stack) { roll_gear(stack); stack.identified = true; };
            event_status_text.clear();
            event_visuals.world_hidden = false;
            startup_log("event started: type=" + std::to_string(static_cast<int>(type)) +
                        ", trigger=" + std::to_string(trigger_id) +
                        ", actions=" + std::to_string(match->action_list));
        };
        // ao.a(boolean): keys 1/3 (ZL/ZR on Switch) call the calm/wild guardian skill.
        const auto summon_guardian = [&](bool calm) {
            if (event_runtime || screen_return_active || menu_active || shop_active || game_over_until) return;
            combat_world.summon_guardian(combat_view(), calm);
        };
        const auto start_attack = [&](bool skill, bool walking) {
            const auto hero = hero_input.state();
            if (hero.partial_x || hero.partial_y) { deferred_attack = skill ? 2 : 1; hero_input.release(); return; }
            int face = hero.direction;
            // n.a(boolean): ao.i() turns toward a side enemy only for an attack from standing (mode 1);
            // one requested while walking is queued (n.h) and starts facing the walk direction.
            if (combat_world.request_attack(combat_view(), skill, walking ? nullptr : &face) && face != hero_input.state().direction)
                hero_input.apply_script_command(6, static_cast<std::uint8_t>(face));
        };
        // ah.a(byte): a boss death starts the type-0 event list of trigger `trigger_id`.
        const auto start_trigger_event = [&](int trigger_id) {
            if (event_runtime) return;
            const auto match = find_event_trigger(event_grid, trigger_id, EventTriggerType::actor_contact,
                persistent_switches, persistent_quests, [&](std::uint8_t a, std::uint8_t b, std::uint8_t count) {
                    return has_inventory_requirement(persistent_inventory, a, b, count);
                });
            if (!match || match->action_list >= event_grid.actions.size()) {
                startup_log("boss trigger " + std::to_string(trigger_id) + " has no active event");
                return;
            }
            active_action_storage = event_grid.actions[match->action_list];
            active_actions = &active_action_storage;
            event_runtime = std::make_unique<EventRuntime>(
                map, event_grid, persistent_switches, persistent_quests,
                persistent_inventory, persistent_money, persistent_vitals, persistent_guardians, persistent_progress);
            event_runtime->item_created = [&](InventoryStack& stack) { roll_gear(stack); stack.identified = true; };
            event_status_text.clear();
            startup_log("boss trigger event started: " + std::to_string(trigger_id));
        };
        const auto try_start_button_event = [&]() {
            if (event_runtime || screen_return_active) return;
            const auto hero = hero_input.state();
            if (hero.motion_mode != 1) {
                interaction_requested = true;
                hero_input.release();
                return;
            }
            const bool pressed_while_walking = interaction_requested;
            interaction_requested = false;
            if (hero.direction < 1 || hero.direction > 4) return;
            static constexpr std::array<int, 5> dx{{0, 0, 0, -1, 1}};
            static constexpr std::array<int, 5> dy{{0, -1, 1, 0, 0}};
            {
                combat::CombatEvents picked;
                const auto view = combat_view();
                const auto add_drop_item = [&](int type, int id, bool apply) {
                    const auto t = static_cast<std::int8_t>(type), i = static_cast<std::int8_t>(id);
                    if (is_stackable_item(std::uint8_t(type)))
                        for (auto& stack : persistent_inventory)
                            if (stack.item_type == t && stack.item_id == i && stack.count + 1 <= inventory_stack_limit) {
                                if (apply) {++stack.count;consolidate_stacks(persistent_inventory);}
                                return true;
                            }
                    if (persistent_inventory.size() >= inventory_slot_limit) return false;
                    if (apply) {
                        // n: dropped gear rolls bonuses; it stays unidentified only when e.a allows it.
                        persistent_inventory.push_back({t, i, 1});
                        auto& added = persistent_inventory.back();
                        roll_gear(added);
                        if (const auto* item = find_catalog_item(item_catalog, t, i))
                            if (const auto info = equipment_info(*item); info && !info->flag) added.identified = true;
                        consolidate_stacks(persistent_inventory);
                    }
                    return true;
                };
                if (combat_world.try_pickup(view, picked, [&](int t, int i) { return add_drop_item(t, i, false); })) {
                    if (picked.gold > 0) {
                        persistent_money += picked.gold;
                        world_notice = tr("Got ","Получено ") + std::to_string(picked.gold) + " " + bank_or(3948,"G");
                    }
                    for (const auto& item : picked.items) {
                        add_drop_item(item.first, item.second, true);
                        const auto* found = find_catalog_item(item_catalog, item.first, item.second);
                        world_notice = tr("Got ","Получено - ") + (found ? shop_text(found->name_key) : tr("item","предмет"));
                    }
                    world_notice_until = SDL_GetTicks() + 1600;
                    return;
                }
                if (!picked.messages.empty()) { world_notice = bank_or(3938,"Inventory is full."); world_notice_until = SDL_GetTicks() + 1600; }
            }
            try_start_event(hero.x_tile, hero.y_tile, EventTriggerType::hero_on_tile);
            if (!event_runtime)
                try_start_event(hero.x_tile + dx[hero.direction],
                                hero.y_tile + dy[hero.direction],
                                EventTriggerType::hero_facing_tile);
            if (!event_runtime) start_attack(false, pressed_while_walking);
        };
        const auto try_skill_attack = [&]() {
            if (event_runtime || screen_return_active || game_over_until) return;
            if (hero_input.state().motion_mode != 1) hero_input.release();
            start_attack(true, hero_input.state().motion_mode == 2);
        };
        bool startup_loading=false;
        const auto confirm_title = [&]() {
            try {
                const auto action=title_menu.confirm();
                if(action==TitleAction::quit) {running=false;return;}
                if(action==TitleAction::continue_game) {
                    const auto* entry=title_menu.selected_save();
                    if(!entry) return;
                    const auto saved=read_save(entry_path(*entry));restore_save(saved);
                    title_menu.active=false;location_started=SDL_GetTicks();
                } else if(action==TitleAction::start_game) startup_loading=true;
            } catch(const std::exception& error) {title_menu.message=error.what();}
        };
        // as.a(int,int) key 0: only while the hero stands (ao.h == 1) on a map of kind 0..14.
        const auto open_minimap = [&]() {
            if(event_runtime || screen_return_active || menu_active || shop_active || service || choice_active ||
               dialogue_active || game_over_until || combat_world.hero_dying()) return;
            if(hero_input.state().motion_mode!=1 || map.kind>14) return;
            if(HEROES_LORE_3DS) return; // the bottom screen shows the minimap all the time
            minimap_open=true;minimap_lit=false;minimap_tick=SDL_GetTicks();
        };
        const auto use_quick_item = [&]() {
            if(event_runtime || screen_return_active) return;
            if(combat_world.hero_dying() || game_over_until) return; // ao.k: ignored in states 5/6
            const auto slot=quick_item_slot(persistent_inventory,quick_item_type);
            if(!slot) world_notice=bank_or(3939,"No item.");
            else {
                // Port quick use: the original spends items only from the bag (ay -> ao.a(ad)) silently;
                // the field notice follows the pickup style "Got - <item>".
                const auto& stack=persistent_inventory[*slot];
                const auto* item=find_catalog_item(item_catalog,stack.item_type,stack.item_id);
                const std::string name=item?shop_text(item->name_key):tr("item","предмет");
                bool antidote=false;
                const bool used=use_consumable(persistent_inventory,*slot,persistent_vitals,&antidote)==ItemUseResult::used;
                if(used && antidote) combat_world.cure_poison();
                world_notice=used?tr("Used - ","Использовано - ")+name:tr("Cannot use ","Нельзя использовать - ")+name;
            }
            world_notice_until=SDL_GetTicks()+1600;
        };
#if !HEROES_LORE_CONSOLE
        const char* snapshot_screen=std::getenv("HEROES_LORE_SNAPSHOT_SCREEN");
        if(snapshot_screen && std::getenv("HEROES_LORE_SNAPSHOT_OUT")) {
            const std::string screen=snapshot_screen;
            title_menu.active=screen.rfind("title",0)==0;
            if(title_menu.active) {
                title_menu.continue_available=screen=="title";title_menu.selection=0;title_menu.message.clear();
                title_menu.page=screen=="title-splash"?TitlePage::splash:screen=="title-logo"?TitlePage::logo:screen.rfind("title-options",0)==0?TitlePage::options:
                    screen=="title-help"?TitlePage::help:screen=="title-about"?TitlePage::reading:
                    screen=="title-character"?TitlePage::character:screen=="title-character-confirm"?TitlePage::character_confirm:
                    screen=="title-guardians"?TitlePage::guardians:
                    screen=="title-load"?TitlePage::load_game:screen=="title-overwrite"?TitlePage::overwrite_warning:
                    (screen.rfind("title-newgame",0)==0 || screen=="title-guardian-confirm")?TitlePage::guardian_confirm:TitlePage::menu;
                title_menu.help_topic=4;
                if(screen=="title-splash") for(int i=0;i<6;++i) title_menu.splash.tick(120,240); // easing in
                if(screen=="title-logo") {
                    int w=0,h=0;SDL_GetRendererOutputSize(renderer,&w,&h);
                    const int ms=std::max(1,std::min(w/240,h/220));
                    for(int i=0;i<24;++i) title_menu.logo.tick(w/ms,h/ms);
                }
                if(screen=="title-options-interface") {title_menu.options_tab=2;title_menu.selection=3;}
                if(screen=="title-load") {
                    title_menu.continue_available=true;
                    title_menu.saves={{7,4,6,true},{6,12,31},{7,3,5},{8,1,0}};title_menu.load_selection=0;
                }
                if(screen.rfind("title-newgame",0)==0 || screen=="title-guardian-confirm") {
                    title_menu.chosen_guardians={true,true,false};title_menu.yes=true;
                    if(screen=="title-newgame7") title_menu.character_class=7;
                    if(screen=="title-newgame8") title_menu.character_class=8;
                }
            }
            item_catalog=load_item_catalog(archive);
            persistent_inventory={{0,7,1},{5,3,1},{6,4,1},{7,0,12},{9,0,3},{4,1,1}};
            persistent_money=3840;
            if(screen=="minimap") {minimap_open=true;minimap_lit=true;minimap_tick=SDL_GetTicks();}
            if(screen=="unequip") {menu_active=screen_return_active=true;menu_page=2;equipment_selection=2;unequip_confirming=true;}
            if(screen=="recipes") {menu_active=screen_return_active=true;menu_page=4;recipes_view=true;for(int slot:{0,1,11}) mark_recipe(persistent_recipes,slot);}
            if(std::getenv("HEROES_LORE_MODERN")) title_menu.wide_ui=title_menu.modern_ui=true; // snapshots of the Modern layout
            if(screen=="inventory" || screen=="equipment-confirm" || screen=="inventory-wrong-class" || screen=="status" || screen=="equipment" || screen=="guardian" || screen=="quest" || screen=="quest-reading" || screen=="save" ||
               screen=="attributes" || screen=="guardian-choice" || screen=="guardian-skill") {
                menu_active=screen_return_active=true;
                menu_page=(screen=="status" || screen=="attributes")?0:screen=="equipment"?2:
                    (screen=="guardian" || screen=="guardian-choice" || screen=="guardian-skill")?3:(screen=="quest" || screen=="quest-reading")?4:screen=="save"?5:1;
                menu_selection=0; bag_selection=0;if(screen=="equipment-confirm") item_action=4;
                if(screen=="attributes") {menu_selection=3;attribute_editing=true;persistent_progress.attribute_points=6;pending_attributes={1,0,2,0};attribute_selection=2;}
                if(screen=="guardian-choice") guardian_choice=1;
                if(screen=="save") {save_confirming=true;}
                if(screen=="guardian-skill") {skill_screen=true;skill_key_calm=false;skill_selection=1;}
                if(screen=="inventory-wrong-class") persistent_inventory[0]={2,1,1};
                if(menu_page==4) {
                    persistent_quests[19/8]|=1u<<(19%8); // Host fixture: Lesley's Reference book 1.
                    quest_reading=screen=="quest-reading";
                }
                refresh_menu();
            } else if(screen=="shop" || screen=="sale" || screen=="buy-confirm" || screen=="sale-confirm") {
                shop_active=screen_return_active=true;shop_catalog=load_shop_catalog(archive);
                shop_selling=screen=="sale" || screen=="sale-confirm";shop_confirming=screen=="buy-confirm" || screen=="sale-confirm";
                shop_quantity=screen=="buy-confirm"?3:screen=="sale-confirm"?2:1;shop_tab=screen=="buy-confirm"?0:1;
                shop_selection=screen=="sale-confirm"?3:2;refresh_shop();
            } else if(screen=="item-message") {
                dialogue_active=item_message_box=true;dialogue_text=bank_or(3938,"Inventory is full.");
            } else if(screen=="choice") {
                choice_active=true;dialogue_text="Would you like to continue your journey?";
            } else if(screen=="ending" || screen=="ending-credits") {
                // Snapshot: Ronin's second page (grey band and face), or the staff roll mid-way.
                start_ending();
                for(int i=0;i<17;++i) ending->tick(240);
                for(int page=0;page<2;++page) {ending->press();ending->tick(240);}
                if(screen=="ending-credits") {ending->phase=EndingSequence::Phase::credits;ending->counter=0;for(int i=0;i<80;++i) ending->tick(240);}
            } else if(screen.rfind("refiner",0)==0 || screen.rfind("blacksmith",0)==0) {
                screen_return_active=true;service_screen=screen.rfind("refiner",0)==0?EventScreen::refiner:EventScreen::blacksmith;
                open_service(*service_screen);
                // Snapshot states: blacksmith-refine / -pick / -identify, refiner-attribute / -mix / -confirm.
                const auto add=[&](InventoryStack stack){return add_item(persistent_inventory,stack);};
                if(screen=="blacksmith-refine" || screen=="blacksmith-pick") {
                    add({11,0,3});service->page=0;service->cursor=1;service->item=100;
                    if(equipped_items[0]) {equipped_items[0]->refine=2;service->cost=200;service->need=3;}
                    if(screen=="blacksmith-pick") {service->picks=equipment_choices(service_inventory(),false,0);service->picking=true;service->pick_title=3;}
                } else if(screen=="blacksmith-identify") {
                    InventoryStack hidden{0,1,1};hidden.identified=false;add(hidden);service->page=1;service->item=int(persistent_inventory.size())-1;
                } else if(screen=="refiner-attribute") {
                    service->page=0;service->cursor=2;service->item=100;service->orb=add({17,4,1});
                } else if(screen=="refiner-mix" || screen=="refiner-confirm") {
                    const int potion=add({7,0,2});service->page=1;service->cursor=3;service->mix={{potion,potion,-1}};
                    service->mix_confirm=screen=="refiner-confirm";
                }
            } else if(screen=="event-flash" || screen=="event-shake") {
                active_action_storage={std::uint8_t(screen=="event-flash"?24:25),0,0,10,0,5,99,0,0};
                active_actions=&active_action_storage;
                event_runtime=std::make_unique<EventRuntime>(map,event_grid);
                const auto effect=event_runtime->step(*active_actions);
                apply_event_visual_command(event_visuals,effect.command,effect.remaining_ticks);
            } else if(screen=="dialogue-chain" || screen=="dialogue-chain-gradual") {
                active_action_storage={1,0,2,9,3,0,50,0,9,1,1,254,99,0,0};
                active_actions=&active_action_storage;
                event_runtime=std::make_unique<EventRuntime>(map,event_grid,persistent_switches,
                    persistent_quests,persistent_inventory,persistent_money,persistent_vitals,persistent_guardians,persistent_progress);
                event_runtime->step(*active_actions);
                dialogue_active=true;dialogue_text="First.";dialogue_portrait=2;
            } else if(screen.rfind("dialogue",0)==0) {
                dialogue_active=true;dialogue_text="The road ahead is long. We should prepare before leaving town.";
                dialogue_portrait=screen=="dialogue-none"?0:screen=="dialogue-right"?-2:2;
                for(const auto& list:event_grid.actions) {
                    bool found=false;
                    for(std::size_t i=0;i+2<list.size();i+=3) if(list[i]==1 && list[i+2]!=0) {
                        dialogue_text=resolve_event_text(event_grid,list[i+1],language_bank).value_or(dialogue_text);
                        const int raw=list[i+2]<128?int(list[i+2]):int(list[i+2])-256;
                        dialogue_portrait=screen=="dialogue-none"?0:(screen=="dialogue-right"?-std::abs(raw):std::abs(raw));
                        found=true;break;
                    }
                    if(found) break;
                }
            }
            if(screen.rfind("dialogue",0)==0) dialogue_reveal.finish();
            if(screen=="dialogue-gradual") dialogue_reveal.end=12;
            if(screen=="dialogue-chain-gradual") dialogue_reveal.end=2;
            if(screen=="level-up") {combat_world.add_level_up_effects();for(int i=0;i<4;++i) combat_world.advance_effects_for_snapshot();}
            if(screen=="combat" || screen=="combat-hit" || screen=="combat-kill") {
                // Map 27 zone 6: hero faces a level-1 enemy (ID 24) one tile north.
                rebuild_combat(current_map_id);
                hero_input.place_on_tile(10,8);
                hero_input.apply_script_command(6,1);
                combat_world.place_enemy(24,24,10,7);
                sync_live_enemies();
                const int swings=screen=="combat"?0:screen=="combat-hit"?3:400;
                for(int i=0;i<swings;++i) {
                    // Drops are random: keep replacing the slain enemy until one drops something.
                    if(screen=="combat-kill" && combat_world.enemies()[0].state==combat::EnemyState::dead) {
                        combat_world.place_enemy(24,24,10,7);sync_live_enemies();
                    }
                    auto view=combat_view();
                    if(screen=="combat-kill" && !combat_world.hero_attacking()) combat_world.request_attack(view,false);
                    if(i==0) combat_world.request_attack(view,false);
                    combat::CombatEvents events;
                    combat_world.tick(view,events);
                    persistent_vitals.hp=view.hp;persistent_vitals.sp=persistent_vitals.max_sp;
                    if(events.experience>0) add_event_experience(persistent_progress,persistent_vitals,persistent_guardians,unsigned(events.experience));
                    sync_live_enemies();
                    if(screen=="combat-kill" && !combat_world.drops().empty()) break;
                }
                if(screen=="combat-kill") hero_input.place_on_tile(10,10); // step back so the drop is visible
                hero_input.sync_world(event_grid,npcs,enemies);
                std::string summary="combat scene: drops="+std::to_string(combat_world.drops().size())+" enemies=";
                for(const auto& e:combat_world.enemies()) summary+=std::to_string(e.sprite)+"@"+std::to_string(e.tile_x)+","+
                    std::to_string(e.tile_y)+"/s"+std::to_string(int(e.state))+"/hp"+std::to_string(e.hp)+" ";
                startup_log(summary+"hero_hp="+std::to_string(persistent_vitals.hp)+" exp="+std::to_string(persistent_progress.experience));
            }
            if(screen.rfind("battle-",0)==0) {
                rebuild_combat(current_map_id);
                hero_input.place_on_tile(10,8);hero_input.apply_script_command(6,1);
                enemy_stats[24].hp=500;enemy_stats[24].move_delay=100;enemy_stats[24].attack_delay=100;
                combat_world.place_enemy(24,24,10,7);
                auto view=combat_view();view.agility=1000;
                if(screen=="battle-status" || screen=="battle-status-bob") {
                    for(int option:{1,5,6,7,0}) combat_world.hit_enemy(0,0,false,1,false,0,option,view);
                    if(screen=="battle-status-bob") {combat::CombatEvents events;combat_world.tick(view,events);}
                } else if(screen=="battle-attack") { // mid-swing: frame layer 7 (c*/i/e trails)
                    int face=1;combat_world.request_attack(view,false,&face);
                    for(int i=0;i<2;++i) {combat::CombatEvents events;combat_world.tick(view,events);}
                } else if(screen=="battle-hurt") {
                    view.agility=-1000;view.defense=0;
                    const int before=view.hp;
                    for(int i=0;i<20 && view.hp==before;++i) combat_world.hit_hero(0,20,2,view);
                    persistent_vitals.hp=view.hp;
                } else {
                    combat_world.hit_enemy(0,(screen=="battle-death"?10000:123)+enemy_stats[24].defense,false,1,false,0,-1,view);
                    const int ticks=screen=="battle-death"?4:screen=="battle-small"?2:0;
                    for(int i=0;i<ticks;++i) {combat::CombatEvents events;combat_world.tick(view,events);}
                }
                sync_live_enemies();hero_input.sync_world(event_grid,npcs,enemies);
            }
            if(screen=="guardian-summon") {
                // Map 27: wait out the first cooldown, summon the calm skill, freeze mid-effect.
                rebuild_combat(current_map_id);
                hero_input.place_on_tile(10,9);hero_input.apply_script_command(6,1);
                combat_world.place_enemy(24,24,10,6);sync_live_enemies();
                bool summoned=false;int after=0;
                for(int i=0;i<200 && after<14;++i) {
                    auto view=combat_view();view.hp=view.max_hp;
                    if(!summoned) summoned=combat_world.summon_guardian(view,true);
                    if(summoned) ++after;
                    combat::CombatEvents events;combat_world.tick(view,events);sync_live_enemies();
                }
                hero_input.sync_world(event_grid,npcs,enemies);
                startup_log("guardian scene: summoned="+std::to_string(summoned)+" state="+std::to_string(combat_world.guardian().state)+
                    " enemy_hp="+std::to_string(combat_world.enemies().empty()?-1:combat_world.enemies()[0].hp));
            }
            if(screen=="boss") {
                // Boss maps 11/13/15/82: stand three tiles below the encounter and let it act.
                for(const auto& e:combat_world.enemies()) if(e.boss!=combat::BossKind::none && !e.ghost) {
                    const int x=e.tile_x,y=std::min(e.tile_y+3,int(map.height)-1);
                    hero_input.place_on_tile(x,y);hero_input.apply_script_command(6,1);break;
                }
                for(int i=0;i<120;++i) {
                    auto view=combat_view();view.hp=view.max_hp;combat::CombatEvents events;
                    combat_world.tick(view,events);persistent_vitals.hp=std::max(1,view.hp);
                    sync_live_enemies();
                    if(i>=40 && combat_world.projectiles().size()>=3) break; // freeze while rays fly
                }
                hero_input.sync_world(event_grid,npcs,enemies);
                std::string summary="boss scene:";
                for(const auto& e:combat_world.enemies()) summary+=" "+std::to_string(int(e.boss))+"@"+std::to_string(e.tile_x)+","+
                    std::to_string(e.tile_y)+"/s"+std::to_string(int(e.state))+"/hp"+std::to_string(e.hp);
                startup_log(summary+" hero_hp="+std::to_string(persistent_vitals.hp));
            }
            if(screen=="world-behind" || screen=="world-front") {
                hero_input.place_on_tile(7,screen=="world-behind"?2:4);
                hero_input.apply_script_command(6,2);
            }
            if(const char* requested=std::getenv("HEROES_LORE_SNAPSHOT_ACTIONS")) {
                const std::string action=requested;
                const int presses=action=="equip"?2:1;
                for(int i=0;i<presses;++i) {
                    SDL_Event input{};input.type=SDL_KEYDOWN;
                    input.key.keysym.sym=action=="cancel"?SDLK_ESCAPE:action=="quick"?SDLK_q:SDLK_RETURN;
                    SDL_PushEvent(&input);
                }
            }
        }
#endif
        Uint32 last_motion_tick = SDL_GetTicks();
        Uint32 last_logo_tick = SDL_GetTicks();
#if !HEROES_LORE_CONSOLE
        if(snapshot_screen && (std::string(snapshot_screen)=="dialogue-chain" ||
            std::string(snapshot_screen).rfind("event-",0)==0)) last_motion_tick-=80;
#endif
#if HEROES_LORE_LOW_POWER
        Uint32 perf_started=SDL_GetTicks(),perf_frames=0,perf_render_ms=0,perf_ticks=0;
#endif
        while (running) {
#if HEROES_LORE_PSP
            psp_poll_controls();
#endif
            while (SDL_PollEvent(&sdl_event)) {
#if HEROES_LORE_3DS
                if(sdl_event.type==SDL_CONTROLLERBUTTONDOWN || sdl_event.type==SDL_CONTROLLERBUTTONUP ||
                   sdl_event.type==SDL_FINGERDOWN || sdl_event.type==SDL_KEYDOWN)
                    ++g_3ds_input_serial; // the bottom-screen menu redraws on input (stick turns arrive as D-pad events;
                                          // raw axis events would count the Circle Pad's jitter)
                { // A worn 3DS button can bounce; with HID sampled several times a frame a press then
                  // arrived twice. Drop a press of a button released less than 60 ms before.
                    static Uint32 released_at[SDL_CONTROLLER_BUTTON_MAX]{};
                    if(sdl_event.type==SDL_CONTROLLERBUTTONUP && sdl_event.cbutton.button<SDL_CONTROLLER_BUTTON_MAX)
                        released_at[sdl_event.cbutton.button]=sdl_event.cbutton.timestamp;
                    if(sdl_event.type==SDL_CONTROLLERBUTTONDOWN && sdl_event.cbutton.button<SDL_CONTROLLER_BUTTON_MAX &&
                       sdl_event.cbutton.padding1!=kTouchTap && // touch steps come in quick series
                       released_at[sdl_event.cbutton.button] && sdl_event.cbutton.timestamp-released_at[sdl_event.cbutton.button]<60) continue;
                }
                if(sdl_event.type==SDL_FINGERDOWN) {
                    // Bottom-screen touch (320x240) becomes the button the touched element shows, so
                    // every screen keeps its own button handling. Layout: draw_bottom_screen (HUD 2x at y 168).
                    const int tx=int(sdl_event.tfinger.x*320.0f),ty=int(sdl_event.tfinger.y*240.0f);
                    const auto press=[&](SDL_GameControllerButton button) {
                        SDL_Event tap{};
                        tap.type=SDL_CONTROLLERBUTTONDOWN;tap.cbutton.state=SDL_PRESSED;tap.cbutton.button=Uint8(button);
                        tap.cbutton.which=controller?SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)):0;
                        tap.cbutton.padding1=kTouchTap;
                        SDL_PushEvent(&tap);
                        tap.type=SDL_CONTROLLERBUTTONUP;tap.cbutton.state=SDL_RELEASED;
                        SDL_PushEvent(&tap);
                    };
                    if(title_menu.active) {
                        // Title menus on the bottom screen: a tap picks a row (or slot), a tap on the
                        // picked one confirms; Options rows change their value by the tapped half.
                        const TitlePage page=title_menu.page;
                        const bool below=page!=TitlePage::splash && page!=TitlePage::logo && page!=TitlePage::loading;
                        const UiHit* hit=nullptr;
                        if(below) for(const auto& h:visual_ui_hits())
                            if((h.kind==12 || h.kind==13) && tx>=h.rect.x && tx<h.rect.x+h.rect.w && ty>=h.rect.y && ty<h.rect.y+h.rect.h) hit=&h;
                        if(hit && hit->kind==13) {title_menu.yes=hit->index==0;press(SDL_CONTROLLER_BUTTON_A);}
                        else if(hit && page==TitlePage::character && title_menu.locked_notice) press(SDL_CONTROLLER_BUTTON_A);
                        else if(hit && hit->selected) {
                            if(page==TitlePage::options) press(tx<160?SDL_CONTROLLER_BUTTON_DPAD_LEFT:SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
                            else press(SDL_CONTROLLER_BUTTON_A);
                        } else if(hit) {
                            if(page==TitlePage::load_game) title_menu.load_selection=hit->index;
                            else if(page==TitlePage::character) title_menu.character_class=6+hit->index;
                            else if(page==TitlePage::guardians) title_menu.guardian_focus=hit->index;
                            else title_menu.selection=hit->index;
                            title_menu.message.clear();
                        }
                        else if(ty>=216 && tx<110) press(SDL_CONTROLLER_BUTTON_A);  // the A hint
                        else if(ty>=216 && tx>=210) press(SDL_CONTROLLER_BUTTON_B); // the B hint: back
                        else if(page==TitlePage::logo || page==TitlePage::splash || page==TitlePage::reading ||
                                page==TitlePage::guardian_intro || page==TitlePage::overwrite_warning) press(SDL_CONTROLLER_BUTTON_A);
                        continue;
                    }
                    const bool field=!title_menu.active && !ending && !menu_active && !shop_active && !service &&
                        !choice_active && !screen_return_active && !dialogue_active && !event_runtime &&
                        event_status_text.empty() && !minimap_open;
                    if(field) {
                        static constexpr SDL_GameControllerButton buttons[]{SDL_CONTROLLER_BUTTON_B,SDL_CONTROLLER_BUTTON_Y,
                            SDL_CONTROLLER_BUTTON_START,SDL_CONTROLLER_BUTTON_LEFTSHOULDER,SDL_CONTROLLER_BUTTON_RIGHTSHOULDER};
                        int hit=-1; // calm guardian, wild guardian, menu, quick item, next item
                        for(int i=0;i<5;++i) {const SDL_Rect r=bottom_button_rect(i);
                            if(tx>=r.x-2 && tx<r.x+r.w+2 && ty>=r.y-2) hit=i;}
                        if(hit>=0) press(buttons[hit]);
                        else if(ty>=148) press(SDL_CONTROLLER_BUTTON_START);              // HP/SP row: menu
                    } else if(minimap_open) press(SDL_CONTROLLER_BUTTON_A);
                    else if(!menu_active && !shop_active && !service && [&]{
                        // Dialogue, Yes/No and messages on the top screen: the buttons the bottom screen shows.
                        for(const auto& h:bottom_screen_hits()) {
                            if(tx<h.rect.x || tx>=h.rect.x+h.rect.w || ty<h.rect.y || ty>=h.rect.y+h.rect.h) continue;
                            if(h.kind==11) press(h.index==0?SDL_CONTROLLER_BUTTON_DPAD_UP:SDL_CONTROLLER_BUTTON_DPAD_DOWN);
                            press(h.kind==7 && h.index==1?SDL_CONTROLLER_BUTTON_B:SDL_CONTROLLER_BUTTON_A);
                            return true;
                        }
                        return false;
                    }()) {}
                    else if((menu_active || shop_active || service) && [&]{
                        // Menu, shop, services: a tab or row the bottom screen drew. Walk the cursor there
                        // with the buttons the menu already understands; a tap on the picked row confirms.
                        const auto& hits=visual_ui_hits();
                        const UiHit* hit=nullptr;
                        for(const auto& h:hits) if(tx>=h.rect.x && tx<h.rect.x+h.rect.w && ty>=h.rect.y && ty<h.rect.y+h.rect.h) hit=&h;
                        if(!hit) return false;
                        if(hit->kind==3 || hit->kind==4) { // attribute points: -/+ on that row
                            attribute_selection=hit->index;
                            if(hit->kind==3 && pending_attributes[std::size_t(hit->index)]>0) --pending_attributes[std::size_t(hit->index)];
                            if(hit->kind==4 && attribute_points_remaining(persistent_progress,pending_attributes)>0)
                                ++pending_attributes[std::size_t(hit->index)];
                            return true;
                        }
                        if(hit->kind==5) {press(SDL_CONTROLLER_BUTTON_A);return true;} // the points box: confirm
                        if(hit->kind==9) {if(!hit->selected) press(SDL_CONTROLLER_BUTTON_DPAD_LEFT);return true;} // guardian key tab
                        if(hit->kind==10) {press(hit->index==0?SDL_CONTROLLER_BUTTON_DPAD_LEFT:SDL_CONTROLLER_BUTTON_DPAD_RIGHT);return true;} // quantity
                        if(hit->kind==7) { // the key buttons under the box
                            static constexpr SDL_GameControllerButton keys[]{SDL_CONTROLLER_BUTTON_A,SDL_CONTROLLER_BUTTON_B,
                                SDL_CONTROLLER_BUTTON_X,SDL_CONTROLLER_BUTTON_Y};
                            press(keys[std::clamp(hit->index,0,3)]);return true;
                        }
                        int current=-1,count=0;
                        for(const auto& h:hits) if(h.kind==hit->kind) {++count;if(h.selected) current=h.index;}
                        if(hit->kind==1) {
                            const int steps=current<0?0:(hit->index-current+count)%count;
                            for(int i=0;i<steps;++i) press(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
                            return true;
                        }
                        if(hit->modern) press(SDL_CONTROLLER_BUTTON_DPAD_RIGHT); // out of the tab column
                        if(current<0 || hit->index==current) {press(SDL_CONTROLLER_BUTTON_A);return true;}
                        const int delta=hit->index-current;
                        for(int i=0;i<std::abs(delta);++i) press(delta>0?SDL_CONTROLLER_BUTTON_DPAD_DOWN:SDL_CONTROLLER_BUTTON_DPAD_UP);
                        return true;
                    }()) {}
                    else if(ty>=216 && tx<110) press(SDL_CONTROLLER_BUTTON_A);            // bottom-left hint
                    else if(ty>=216 && tx>=210) press(SDL_CONTROLLER_BUTTON_B);           // bottom-right hint
                    else if(!menu_active && !shop_active && !service && !title_menu.active) press(SDL_CONTROLLER_BUTTON_A); // dialogue, notices
                    continue;
                }
#endif
#if HEROES_LORE_PSP
                if((sdl_event.type==SDL_CONTROLLERBUTTONDOWN || sdl_event.type==SDL_CONTROLLERBUTTONUP) && sdl_event.cbutton.which!=-4242) continue;
                if(sdl_event.type==SDL_CONTROLLERAXISMOTION) continue;

#endif
#if !HEROES_LORE_CONSOLE
                if (sdl_event.type == SDL_KEYDOWN) button_style = ButtonStyle::keyboard;
                else if (sdl_event.type == SDL_CONTROLLERBUTTONDOWN ||
                         (sdl_event.type == SDL_CONTROLLERAXISMOTION && std::abs(sdl_event.caxis.value) > 16000))
                    button_style = pad_style(SDL_GameControllerFromInstanceID(
                        sdl_event.type == SDL_CONTROLLERBUTTONDOWN ? sdl_event.cbutton.which : sdl_event.caxis.which));
                // Desktop: F11 or Alt+Enter switches between the window and borderless fullscreen.
                if (sdl_event.type == SDL_KEYDOWN && !sdl_event.key.repeat &&
                    (sdl_event.key.keysym.sym == SDLK_F11 ||
                     (sdl_event.key.keysym.sym == SDLK_RETURN && (sdl_event.key.keysym.mod & KMOD_ALT)))) {
                    const bool full = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP;
                    SDL_SetWindowFullscreen(window, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    continue;
                }
#endif
                if (logged_events < 8) {
                    startup_log("SDL event type: " + std::to_string(sdl_event.type));
                    ++logged_events;
                }
                // Left stick: turn direction changes into D-pad presses/releases so every screen
                // (title, menus, shops, services, field) handles it like the D-pad.
                if(sdl_event.type==SDL_CONTROLLERAXISMOTION &&
                   (sdl_event.caxis.axis==SDL_CONTROLLER_AXIS_LEFTX || sdl_event.caxis.axis==SDL_CONTROLLER_AXIS_LEFTY)) {
                    SDL_GameController* pad=SDL_GameControllerFromInstanceID(sdl_event.caxis.which);
                    if(!pad) continue;
                    const int wanted=direction_from_stick(SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_LEFTX),
                        SDL_GameControllerGetAxis(pad,SDL_CONTROLLER_AXIS_LEFTY),stick_direction);
                    if(wanted==stick_direction) continue;
                    SDL_Event synthetic{};
                    synthetic.cbutton.which=sdl_event.caxis.which;
                    if(stick_direction) {
                        synthetic.type=SDL_CONTROLLERBUTTONUP;synthetic.cbutton.state=SDL_RELEASED;
                        synthetic.cbutton.button=controller_button_for_direction(stick_direction);
                        SDL_PushEvent(&synthetic);
                    }
                    stick_direction=wanted;
                    if(wanted) {
                        synthetic.type=SDL_CONTROLLERBUTTONDOWN;synthetic.cbutton.state=SDL_PRESSED;
                        synthetic.cbutton.button=controller_button_for_direction(wanted);
                        SDL_PushEvent(&synthetic);
                    }
                    continue;
                }
                if(title_menu.active && (sdl_event.type==SDL_KEYDOWN || sdl_event.type==SDL_CONTROLLERBUTTONDOWN)) {
                    if(sdl_event.type==SDL_KEYDOWN && sdl_event.key.repeat) continue;
                    const bool keyboard=sdl_event.type==SDL_KEYDOWN;
                    const auto button=keyboard?Uint8(0):labelled_controller_button(sdl_event.cbutton.button);
                    const int direction=keyboard?direction_from_key(sdl_event.key.keysym.sym):direction_from_controller_button(button);
                    // bg: PRESS ANY KEY -- every key or button leaves the logo.
                    if(title_menu.page==TitlePage::logo) {title_menu.open_menu();continue;}
                    if(title_menu.page==TitlePage::splash) {title_menu.page=TitlePage::logo;continue;} // bg state 10: any key
                    const auto previous_page=title_menu.page;
                    const PortSettings previous{title_menu.game_speed,title_menu.instant_speech,title_menu.smooth_camera,title_menu.language,title_menu.heroes_unlocked,title_menu.sound,title_menu.soundfont,title_menu.smooth_motion,title_menu.wide_ui,title_menu.autosave,title_menu.modern_ui,title_menu.rumble};
                    title_menu.navigate(direction);
                    // Port: L/R (Q/E, PageUp/PageDown) turn the Options tabs from any row.
                    if(title_menu.page==TitlePage::options) {
                        const auto key=keyboard?sdl_event.key.keysym.sym:SDLK_UNKNOWN;
                        if(button==SDL_CONTROLLER_BUTTON_LEFTSHOULDER || key==SDLK_q || key==SDLK_PAGEUP) title_menu.switch_options_tab(-1);
                        if(button==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER || key==SDLK_e || key==SDLK_PAGEDOWN) title_menu.switch_options_tab(1);
                    }
                    const bool confirm=keyboard?(sdl_event.key.keysym.sym==SDLK_RETURN || sdl_event.key.keysym.sym==SDLK_SPACE):
                        (button==SDL_CONTROLLER_BUTTON_A || button==SDL_CONTROLLER_BUTTON_START);
                    const bool back=keyboard?sdl_event.key.keysym.sym==SDLK_ESCAPE:button==SDL_CONTROLLER_BUTTON_B;
                    if(back) title_menu.back();
                    if(confirm) confirm_title();
                    if(previous.smooth_camera!=title_menu.smooth_camera) world_camera.reset();
                    if(settings_enabled && previous_page==TitlePage::options &&
                       (previous.game_speed!=title_menu.game_speed || previous.instant_speech!=title_menu.instant_speech ||
                        previous.smooth_camera!=title_menu.smooth_camera || previous.language!=title_menu.language || previous.sound!=title_menu.sound || previous.soundfont!=title_menu.soundfont || previous.smooth_motion!=title_menu.smooth_motion || previous.wide_ui!=title_menu.wide_ui || previous.autosave!=title_menu.autosave || previous.modern_ui!=title_menu.modern_ui || previous.rumble!=title_menu.rumble ||
                        title_menu.page!=previous_page)) {
                        try {write_port_settings(settings_path,{title_menu.game_speed,title_menu.instant_speech,title_menu.smooth_camera,title_menu.language,title_menu.heroes_unlocked,title_menu.sound,title_menu.soundfont,title_menu.smooth_motion,title_menu.wide_ui,title_menu.autosave,title_menu.modern_ui,title_menu.rumble});}
                        catch(const std::exception& error) {title_menu.message=std::string("Settings: ")+error.what();}
                    }
                    if(previous.language!=title_menu.language) reload_language(title_menu.language);
                    if(!previous.rumble && title_menu.rumble) pad_rumble(0x6000,0x6000,250,"test"); // feel it when turning it on
                    continue;
                }
                if (sdl_event.type == SDL_QUIT) {
                    startup_log("SDL_QUIT received");
                    running = false;
                } else if (sdl_event.type == SDL_APP_WILLENTERBACKGROUND ||
                           (sdl_event.type == SDL_WINDOWEVENT && sdl_event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)) {
                    // as.hideNotify: sound pauses, keys are released and the field opens the game menu.
                    if (audio_device.id) SDL_PauseAudioDevice(audio_device.id, 1);
                    hero_input.release();
                    if (!title_menu.active && !ending && !menu_active && !shop_active && !service && !minimap_open &&
                        !event_runtime && !screen_return_active && !game_over_until) open_menu();
                } else if (sdl_event.type == SDL_APP_DIDENTERFOREGROUND ||
                           (sdl_event.type == SDL_WINDOWEVENT && sdl_event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)) {
                    if (audio_device.id) SDL_PauseAudioDevice(audio_device.id, 0); // as.showNotify: bw.b()
                } else if (minimap_open && ((sdl_event.type == SDL_KEYDOWN && !sdl_event.key.repeat) ||
                                            sdl_event.type == SDL_CONTROLLERBUTTONDOWN)) {
                    minimap_open = false; // as.keyPressed state 11: any key returns to the field
                } else if (ending && ((sdl_event.type == SDL_KEYDOWN && !sdl_event.key.repeat) ||
                                      sdl_event.type == SDL_CONTROLLERBUTTONDOWN)) {
                    ending->press(); // as.keyPressed state 12: any key turns the story page
                } else if (service && sdl_event.type == SDL_KEYDOWN && !sdl_event.key.repeat) {
                    const auto key = sdl_event.key.keysym.sym;
                    service_input(direction_from_key(key), key == SDLK_RETURN || key == SDLK_SPACE,
                                  key == SDLK_ESCAPE || key == SDLK_BACKSPACE);
                } else if (service && sdl_event.type == SDL_CONTROLLERBUTTONDOWN) {
                    const auto button = labelled_controller_button(sdl_event.cbutton.button);
                    service_input(direction_from_controller_button(button), button == SDL_CONTROLLER_BUTTON_A,
                                  button == SDL_CONTROLLER_BUTTON_B);
                } else if (sdl_event.type == SDL_KEYDOWN && !sdl_event.key.repeat) {
                    const int direction = direction_from_key(sdl_event.key.keysym.sym);
                    if(direction && choice_active) choice_selected_yes=(direction==1 || direction==3);
                    else if (direction && menu_active) navigate_menu(direction);
                    else if (direction && shop_active) navigate_shop(direction);
                    else if (direction && !event_runtime && !screen_return_active)
                        hero_input.press(direction);
                    else if (sdl_event.key.keysym.sym == SDLK_x && (shop_active || menu_active || choice_active || screen_return_active || !event_status_text.empty())) page_shop_text();
                    else if (sdl_event.key.keysym.sym == SDLK_x && !dialogue_active && !event_runtime) try_skill_attack();
                    else if ((sdl_event.key.keysym.sym == SDLK_1 || sdl_event.key.keysym.sym == SDLK_3) &&
                             !dialogue_active && !event_runtime) summon_guardian(sdl_event.key.keysym.sym == SDLK_1);
                    else if (sdl_event.key.keysym.sym == SDLK_TAB && shop_active) toggle_shop_mode();
                    else if (sdl_event.key.keysym.sym == SDLK_TAB && menu_active && menu_page == 4 && !quest_reading) {recipes_view = !recipes_view;recipe_selection = 0;refresh_menu();}
                    else if (sdl_event.key.keysym.sym == SDLK_q && !event_runtime && !screen_return_active) use_quick_item();
                    else if (sdl_event.key.keysym.sym == SDLK_e && !event_runtime && !screen_return_active)
                        quick_item_type=7+(quick_item_type-7+1)%4;
                    else if (sdl_event.key.keysym.sym == SDLK_0 || sdl_event.key.keysym.sym == SDLK_m) open_minimap();
                    else if (sdl_event.key.keysym.sym == SDLK_RETURN ||
                             sdl_event.key.keysym.sym == SDLK_SPACE) {
                        if (menu_active) confirm_menu();
                        else if (shop_active) confirm_shop(); else if (choice_active) choice_response = choice_selected_yes;
                        else if (dialogue_active) {
                            advance_dialogue();
                        } else try_start_button_event();
                    } else if (sdl_event.key.keysym.sym == SDLK_ESCAPE ||
                               sdl_event.key.keysym.sym == SDLK_BACKSPACE) {
                        if (menu_active) back_menu();
                        else if (shop_active && (!shop_notice.empty() || shop_asking || shop_confirming)) {
                            if (!shop_notice.empty()) shop_notice.clear();
                            else if (shop_asking) shop_asking = false;
                            else shop_confirming = false;
                            refresh_shop();
                        } else if (screen_return_active) {
                            screen_return_active = false;
                            service_screen.reset();
                            shop_active = false;
                            dialogue_scroll_lines = 0;
                            event_status_text.clear();
                        } else if (choice_active) choice_response = false;
                        else if(event_runtime) event_runtime->request_skip();
                        else open_menu();
                    }
                } else if (sdl_event.type == SDL_KEYUP) {
                    if (!event_runtime && !screen_return_active &&
                        direction_from_key(sdl_event.key.keysym.sym))
                        hero_input.release();
                } else if (sdl_event.type == SDL_CONTROLLERBUTTONDOWN) {
                    const auto button = labelled_controller_button(sdl_event.cbutton.button);
                    const int direction = direction_from_controller_button(button);
                    // Direct face buttons apply only on the field; menu/dialogue actions stay contextual.
                    if ((button == SDL_CONTROLLER_BUTTON_B || button == SDL_CONTROLLER_BUTTON_Y) &&
                        !menu_active && !shop_active && !choice_active && !screen_return_active &&
                        !dialogue_active && !event_runtime && event_status_text.empty() && !minimap_open) {
                        summon_guardian(button == SDL_CONTROLLER_BUTTON_B);
                        continue;
                    }
                    if(direction && choice_active) choice_selected_yes=(direction==1 || direction==3);
                    else if (direction && menu_active) navigate_menu(direction);
                    else if (direction && shop_active) navigate_shop(direction);
                    else if (direction && !event_runtime && !screen_return_active)
                        hero_input.press(direction);
                    else if (button == SDL_CONTROLLER_BUTTON_X && (shop_active || menu_active || choice_active || screen_return_active || !event_status_text.empty())) page_shop_text();
                    else if (button == SDL_CONTROLLER_BUTTON_X && !dialogue_active && !event_runtime) try_skill_attack();
                    else if (button == SDL_CONTROLLER_BUTTON_START) {
                        if (menu_active) close_menu(); else open_menu();
                    }
                    else if (button == SDL_CONTROLLER_BUTTON_BACK) open_minimap(); // Select / − : the phone's 0 key
                    // Port: L/R page through the menu and shop tabs; on the field they keep the quick item.
                    else if((button==SDL_CONTROLLER_BUTTON_LEFTSHOULDER || button==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) &&
                            (menu_active || shop_active)) {
                        const int step=button==SDL_CONTROLLER_BUTTON_LEFTSHOULDER?3:4;
                        if(shop_active) navigate_shop(step,true);
                        else if(menu_tabs_free()) navigate_menu(step,true);
                    }
                    else if(button==SDL_CONTROLLER_BUTTON_LEFTSHOULDER && !event_runtime && !screen_return_active) use_quick_item();
                    else if(button==SDL_CONTROLLER_BUTTON_RIGHTSHOULDER && !event_runtime && !screen_return_active)
                        quick_item_type=7+(quick_item_type-7+1)%4;
                    else if (button == SDL_CONTROLLER_BUTTON_Y && shop_active) toggle_shop_mode();
                    else if (button == SDL_CONTROLLER_BUTTON_Y && menu_active && menu_page == 4 && !quest_reading) {recipes_view = !recipes_view;recipe_selection = 0;refresh_menu();}
                    else if (button == SDL_CONTROLLER_BUTTON_A) {
                        if (menu_active) confirm_menu();
                        else if (shop_active) confirm_shop(); else if (choice_active) choice_response = choice_selected_yes;
                        else if (dialogue_active) {
                            advance_dialogue();
                        } else try_start_button_event();
                    } else if (button == SDL_CONTROLLER_BUTTON_B) {
                        if (menu_active) back_menu();
                        else if (shop_active && (!shop_notice.empty() || shop_asking || shop_confirming)) {
                            if (!shop_notice.empty()) shop_notice.clear();
                            else if (shop_asking) shop_asking = false;
                            else shop_confirming = false;
                            refresh_shop();
                        } else if (screen_return_active) {
                            screen_return_active = false;
                            service_screen.reset();
                            shop_active = false;
                            dialogue_scroll_lines = 0;
                            event_status_text.clear();
                        } else if (choice_active) choice_response = false;
                        else if(event_runtime) event_runtime->request_skip();
                    }
                } else if (sdl_event.type == SDL_CONTROLLERAXISMOTION &&
                           (sdl_event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
                            sdl_event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)) {
                    const bool left = sdl_event.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT;
                    bool& held = left ? trigger_left_held : trigger_right_held;
                    if (sdl_event.caxis.value > 16000 && !held) {
                        held = true;
                        if (!dialogue_active && !event_runtime) summon_guardian(left);
                    } else if (sdl_event.caxis.value < 8000) held = false;
#if HEROES_LORE_VITA
                // The Vita has no L2/R2: the rear touchpad's left / right half or the right stick
                // flicked left / right summons the calm / wild guardian (shown as L2 / R2).
                } else if (sdl_event.type == SDL_CONTROLLERAXISMOTION && sdl_event.caxis.axis == SDL_CONTROLLER_AXIS_RIGHTX) {
                    const int value = sdl_event.caxis.value;
                    if (value < -24000 && !trigger_left_held) {
                        trigger_left_held = true;
                        if (!dialogue_active && !event_runtime) summon_guardian(true);
                    } else if (value > 24000 && !trigger_right_held) {
                        trigger_right_held = true;
                        if (!dialogue_active && !event_runtime) summon_guardian(false);
                    } else if (std::abs(value) < 8000) trigger_left_held = trigger_right_held = false;
                } else if (sdl_event.type == SDL_FINGERDOWN && sdl_event.tfinger.touchId == 1) { // 1: rear pad
                    if (!dialogue_active && !event_runtime) summon_guardian(sdl_event.tfinger.x < 0.5f);
#endif
                } else if (sdl_event.type == SDL_CONTROLLERBUTTONUP &&
                           !event_runtime &&
                           direction_from_controller_button(sdl_event.cbutton.button)) {
                    hero_input.release();
                } else if (sdl_event.type == SDL_CONTROLLERDEVICEADDED && !controller &&
                           SDL_IsGameController(sdl_event.cdevice.which)) {
                    controller = SDL_GameControllerOpen(sdl_event.cdevice.which);
                    if(controller) startup_log("controller added: "+std::string(SDL_GameControllerName(controller))+", rumble="+std::to_string(int(SDL_GameControllerHasRumble(controller))));
                } else if (sdl_event.type == SDL_CONTROLLERDEVICEREMOVED && controller &&
                           SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) ==
                               sdl_event.cdevice.which) {
                    SDL_GameControllerClose(controller);
                    controller = nullptr;
                }
            }
            // A release can be lost (it arrives during a scene, a menu or a screen and is ignored there),
            // which left the hero walking with nothing held. Reconcile with the real D-pad/arrow state:
            // stop when nothing is held, keep the walking direction while it is held, otherwise turn.
            if(!title_menu.active && !event_runtime && !screen_return_active && !menu_active && !shop_active &&
               hero_input.state().motion_mode==2) {
                const Uint8* keys=SDL_GetKeyboardState(nullptr);
                const auto held=[&](int direction) {
                    static constexpr SDL_Scancode scancodes[]{SDL_SCANCODE_UP,SDL_SCANCODE_DOWN,SDL_SCANCODE_LEFT,SDL_SCANCODE_RIGHT};
                    static constexpr SDL_GameControllerButton buttons[]{SDL_CONTROLLER_BUTTON_DPAD_UP,SDL_CONTROLLER_BUTTON_DPAD_DOWN,
                        SDL_CONTROLLER_BUTTON_DPAD_LEFT,SDL_CONTROLLER_BUTTON_DPAD_RIGHT};
                    return
#if HEROES_LORE_PSP
                           psp_button_held(buttons[direction-1]) ||
#endif
                           (keys && keys[scancodes[direction-1]]) || stick_direction==direction ||
                           (controller && SDL_GameControllerGetButton(controller,buttons[direction-1]));
                };
                const int walking=hero_input.state().direction;
                int wanted=walking>=1 && walking<=4 && held(walking)?walking:0;
                for(int direction=1;direction<=4 && !wanted;++direction) if(held(direction)) wanted=direction;
                if(!wanted) hero_input.release();
                else if(wanted!=walking) hero_input.press(wanted);
            }
            // at: "Under refining." stays up 500 ms (1500 ms on success) before the outcome; a
            // destroyed device saves the game (n.o()).
            if(service && service->busy) {
                if(!service_busy_until) service_busy_until=SDL_GetTicks()+(service->success?1500:500);
                else if(SDL_GetTicks()>=service_busy_until) {
                    service_busy_until=0;
                    service->finish_refine(service_inventory());
                    if(service->autosave && map_allows_save(current_map_id))
                        try {write_save(save_path_for(new_game.character_class),capture_save());}
                        catch(const std::exception& error) {startup_log(std::string("autosave failed: ")+error.what());}
                    service->autosave=false;
                }
            }
            // Port autosave: after the entry script, menus and any step in progress have finished.
            if(autosave_pending && (!title_menu.autosave || !map_allows_save(current_map_id))) autosave_pending=false;
            if(autosave_pending && !event_runtime && !title_menu.active && !menu_active && !screen_return_active &&
               !shop_active && !service && !game_over_until && !combat_world.hero_dying() && persistent_vitals.hp>0) {
                const auto hero=hero_input.state();
                if(hero.motion_mode==1 && !hero.partial_x && !hero.partial_y) {
                    autosave_pending=false;
                    try {write_save(autosave_path,capture_save());}
                    catch(const std::exception& error) {startup_log(std::string("autosave failed: ")+error.what());}
                }
            }
            // ao.e(0): a new active guardian reloads the weapon with its element's glow.
            hero_assets.peaceful=map.kind==1 || map.kind==5 || map.kind==9 || map.kind==15;
            if(!event_runtime && !title_menu.active) {
                int element=0;
                if(persistent_guardians.equipped<persistent_guardians.slots.size())
                    if(const auto& active=persistent_guardians.slots[persistent_guardians.equipped]) element=active->id%3+1;
                if(element!=hero_assets.element) {
                    try {
                        auto next=load_hero_textures(renderer,archive,new_game.character_class,equipped_items,element);
                        destroy_textures(hero_assets.textures);hero_assets=std::move(next);
                    } catch(const std::exception& error) {startup_log(std::string("weapon glow reload failed: ")+error.what());hero_assets.element=element;}
                }
            }
            // bw: title 22, ending 23, otherwise ae.h by tileset (maps 79..81: 4); silent at game over.
            {
                int wanted=-1;
                if(ending) wanted=23;
                else if(title_menu.active && title_menu.page==TitlePage::splash) wanted=-1; // 22 starts in bg.c()
                else if(title_menu.active && !title_menu.in_game) wanted=22;
                else if(!game_over_until) wanted=current_map_id>=79 && current_map_id<=81?4:
                    map.kind<20?kMapMusic[map.kind]:-1;
                if(wanted!=current_music) {current_music=wanted;play_music(AudioMixer::map_music,wanted);}
                if(!event_runtime && audio.music_playing(AudioMixer::event_music)) audio.stop_music(AudioMixer::event_music); // ah.c()
                audio.set_enabled(title_menu.sound);
                static int frames_shown=0;
                if(++frames_shown==3) // after the first frames are on screen: unpacking a sound on its
                    for(int id=0;id<32;++id) load_sound(id); // first use stalled the frame (guardian hits)
                if(soundfont==kEmbeddedSoundFont && frames_shown>2) {
                    // Built in: about 6 MB to inflate, quick enough for the main thread on every platform.
                    const void* embedded=nullptr;std::size_t embedded_size=0;
                    const bool loaded=embedded_soundfont(&embedded,&embedded_size) && audio.load_soundfont_compressed(embedded,embedded_size);
                    startup_log(loaded?"Embedded SoundFont loaded":"Embedded SoundFont load failed");
                    soundfont.clear();
                }
                if(!soundfont.empty() && frames_shown>2) {
#if HEROES_LORE_SWITCH
                    // libnx crashes when a worker thread exits, so Switch reads it here (a few seconds).
                    startup_log(audio.load_soundfont(soundfont)?"SoundFont loaded":"SoundFont load failed: "+soundfont);
#else
                    // Elsewhere a worker reads it: on a Vita memory card the 30 MB took ~20 s on the logo.
                    struct SoundFontJob {AudioMixer* audio; std::string path;};
                    auto* job=new SoundFontJob{&audio,soundfont};
                    if(SDL_Thread* loader=SDL_CreateThread([](void* data) -> int {
                           const std::unique_ptr<SoundFontJob> owned(static_cast<SoundFontJob*>(data));
                           const bool loaded=owned->audio->load_soundfont(owned->path);
                           startup_log(loaded?"SoundFont loaded":"SoundFont load failed: "+owned->path);
                           return loaded?0:1;
                       },"soundfont",job))
                        SDL_DetachThread(loader);
                    else {delete job;startup_log("SoundFont thread failed: "+std::string(SDL_GetError()));}
#endif
                    soundfont.clear();
                }
                title_menu.soundfont_available=audio.has_soundfont();
                audio.set_soundfont_enabled(title_menu.soundfont);
#if HEROES_LORE_PSP
                static int reported_backend=-1;
                const int backend=title_menu.soundfont_available && title_menu.soundfont;
                if(backend!=reported_backend) {
                    startup_log(backend?"PSP music backend: embedded SoundFont":"PSP music backend: builtin fallback");
                    reported_backend=backend;
                }
#endif
            }
            // ao.n runs after every equipment change: keep max HP/SP in step with gear bonuses.
            if(!event_runtime) {
                if(gear_totals(equipped_items,item_catalog).bonus!=persistent_progress.gear_bonus) sync_derived_stats();
            }
            const Uint32 now = SDL_GetTicks();
            if(title_menu.active) last_motion_tick=now;
            if(title_menu.active && title_menu.page==TitlePage::splash && now-last_logo_tick>=20) { // bs.a(20): 20 ms frames
                int w=0,h=0;SDL_GetRendererOutputSize(renderer,&w,&h);
                const int ms=std::max(1,std::min(w/240,h/220));
                if(HEROES_LORE_LOW_POWER) {
                    int steps=0;
                    do {title_menu.splash.tick(h/ms/2,h/ms);last_logo_tick=advance_logic_tick(last_logo_tick,now,20);} while(now-last_logo_tick>=20 && ++steps<4);
                } else {title_menu.splash.tick(h/ms/2,h/ms);last_logo_tick=now;}
                if(title_menu.splash.done) title_menu.page=TitlePage::logo;
            }
            if(title_menu.active && (title_menu.page==TitlePage::menu || title_menu.page==TitlePage::load_game) &&
               now-last_logo_tick>=title_menu.tick_ms()) {
                title_menu.tick_brush();last_logo_tick=HEROES_LORE_LOW_POWER?advance_logic_tick(last_logo_tick,now,title_menu.tick_ms()):now;
            }
            if(title_menu.active && title_menu.page==TitlePage::logo && now-last_logo_tick>=title_menu.tick_ms()) {
                int w=0,h=0;SDL_GetRendererOutputSize(renderer,&w,&h);
                const int ms=std::max(1,std::min(w/240,h/220));
                title_menu.logo.tick(w/ms,h/ms);last_logo_tick=HEROES_LORE_LOW_POWER?advance_logic_tick(last_logo_tick,now,title_menu.tick_ms()):now;
            }
#if HEROES_LORE_3DS
            // The logo page stays on the top screen while the menus are below: its feathers keep falling.
            static Uint32 last_feather_tick=0;
            if(title_menu.active && !title_menu.in_game && title_menu.page!=TitlePage::logo && title_menu.page!=TitlePage::splash &&
               title_menu.page!=TitlePage::loading && now-last_feather_tick>=title_menu.tick_ms()) {
                int w=0,h=0;SDL_GetRendererOutputSize(renderer,&w,&h);
                const int ms=std::max(1,std::min(w/240,h/220));
                title_menu.logo.tick(w/ms,h/ms);last_feather_tick=now;
            }
#endif
            if (ending && now - last_motion_tick >= title_menu.tick_ms()) {
                int w=0,h=0;SDL_GetRendererOutputSize(renderer,&w,&h);
                ending->tick(h/std::max(1,std::min(w/240,h/220)));
                if (ending->phase == EndingSequence::Phase::done) finish_ending();
                last_motion_tick = HEROES_LORE_LOW_POWER?advance_logic_tick(last_motion_tick,now,title_menu.tick_ms()):now;
#if HEROES_LORE_LOW_POWER
                ++perf_ticks;
#endif
            }
            // ah Skip bypasses bs.b()'s frame limiter: the scene fast-forwards one logic tick per frame.
            // State 11 freezes the field; ae.d flips on every painted frame (one per logic tick).
            if (minimap_open && now - minimap_tick >= title_menu.tick_ms()) {minimap_lit = !minimap_lit; minimap_tick = now;}
            if (!title_menu.active && !ending && !minimap_open && (now - last_motion_tick >= title_menu.tick_ms() ||
                                                  (event_runtime && event_runtime->skipping()))) {
                // Positions before this tick: the renderer glides from them to the new ones.
                motion_before.hero={hero_input.state().x_pixels,hero_input.state().y_pixels};
                motion_before.npcs.clear();
                for(const auto& m:npc_motion) motion_before.npcs.push_back({m.x_pixels,m.y_pixels});
                motion_before.enemies.clear();
                for(const auto& e:combat_world.enemies()) motion_before.enemies.push_back({e.x_px,e.y_px});
                motion_before.camera=event_runtime?SDL_Point{event_runtime->camera.x,event_runtime->camera.y}:SDL_Point{0,0};
                motion_before.view=world_camera.view;
                motion_before.map=current_map_id;
                if (event_runtime) { // ah.b(): pan a detached camera, otherwise keep it on the hero
                    auto& camera = event_runtime->camera;
                    if (camera.follow) { camera.x = hero_input.state().x_pixels; camera.y = hero_input.state().y_pixels; }
                    else camera.tick();
                }
                bool event_effect_active = false;
                if (event_runtime && active_actions && event_status_text.empty()) {
                    // Pump synchronous commands to the next real wait before rendering.
                    // In particular, acknowledging TALKTEXT must not expose a world frame
                    // before the following TALKTEXT has opened. Bound branches per tick.
                    for (unsigned event_steps = 0; event_steps < 64 && event_runtime &&
                         active_actions && event_status_text.empty(); ++event_steps) {
                        const auto result = event_runtime->step(*active_actions, choice_response,
                            dialogue_advance_requested, event_steps == 0 ? 1 : 0);
                        choice_response.reset();
                        dialogue_advance_requested = false;
                        // ah state 5 (FLASH/SHAKE) runs h() without n.n(): the world stays frozen.
                        event_effect_active = result.status == EventStepStatus::visual_command &&
                            (result.command.opcode == 24 || result.command.opcode == 25);
                        if (result.command.opcode == 8 && result.status == EventStepStatus::applied) {
                            // ah.s: ao.a(SS) moves the hero to the tile; motion and facing are kept.
                            const auto hero = hero_input.state();
                            hero_input.displace(result.command.arg1 * 16 - hero.x_pixels,
                                                result.command.arg2 * 16 - hero.y_pixels);
                        } else if (result.command.opcode == 41 &&
                            result.status == EventStepStatus::applied) {
                            const auto index = static_cast<std::size_t>(result.command.arg1);
                            if (index < npcs.size()) {
                                const auto old_x = npcs[index].x;
                                const auto old_y = npcs[index].y;
                                const auto old_partial_x = npcs[index].partial_x;
                                const auto old_partial_y = npcs[index].partial_y;
                                auto decoded = decode_npcs(event_grid);
                                if (index < decoded.size()) {
                                    npcs[index] = decoded[index];
                                    npcs[index].x = old_x;
                                    npcs[index].y = old_y;
                                    npcs[index].partial_x = old_partial_x;
                                    npcs[index].partial_y = old_partial_y;
                                    try {
                                        const std::vector<MapEntity> changed_npc{npcs[index]};
                                        auto added = load_npc_textures(
                                            renderer, archive, changed_npc);
                                        for (auto it = added.textures.begin();
                                             it != added.textures.end();) {
                                            const auto existing = npc_assets.textures.find(it->first);
                                            if (existing != npc_assets.textures.end()) {
                                                SDL_DestroyTexture(it->second);
                                                it = added.textures.erase(it);
                                            } else ++it;
                                        }
                                        npc_assets.textures.insert(added.textures.begin(),
                                                                   added.textures.end());
                                        added.textures.clear();
                                        for (auto& animation : added.animations)
                                            npc_assets.animations.emplace(animation.first,
                                                                          std::move(animation.second));
                                    } catch (const std::exception& error) {
                                        event_status_text = std::string("NPC sprite update failed: ") +
                                                            error.what();
                                    }
                                }
                            }
                        } else if (result.command.opcode == 42) {
                            const auto cell = static_cast<std::size_t>(result.command.arg2) * map.width +
                                              result.command.arg1;
                            const auto value = cell < map.terrain.size()
                                ? static_cast<int>(map.terrain[cell]) : -1;
                            char stem[16];
                            std::snprintf(stem, sizeof(stem), "m/t/t%02u", unsigned(map.kind));
                            ensure_sprite_texture(renderer, archive, stem, value, terrain_textures);
                            // ah.t writes the shared ae collision: the combat grid must see it too.
                            if (cell < event_grid.overlay.size())
                                combat_world.set_collision(int(result.command.arg1), int(result.command.arg2),
                                                           static_cast<std::int8_t>(event_grid.overlay[cell]));
                        } else if (result.command.opcode == 40) {
                            objects = decode_objects(event_grid);
                            char stem[16];
                            std::snprintf(stem, sizeof(stem), "m/t/o%02u", unsigned(map.kind));
                            for (const auto& object : objects)
                                if (object.resource_slot >= 0)
                                    ensure_sprite_texture(renderer, archive, stem,
                                                          object.frame_index, object_textures);
                        }
                        if (result.status == EventStepStatus::wait && result.command.opcode == 1) {
                            choice_active = false;
                            if (!dialogue_active) {
                                const auto text = resolve_event_text(event_grid, result.command.arg1,
                                                                     language_bank);
                                dialogue_text = text.value_or("[missing event text]");
                                // Port: a "[Combination]" line enters the recipe journal.
                                if (const auto index = event_text_bank_index(event_grid, result.command.arg1))
                                    mark_recipe(persistent_recipes, recipe_slot(*index));
                                dialogue_portrait = result.command.arg2 < 128 ? int(result.command.arg2)
                                    : int(result.command.arg2)-256;
                                dialogue_scroll_lines = 0;
                                dialogue_active = true;dialogue_reveal.reset();item_message_box = false;
                                startup_log("dialogue portrait="+std::to_string(dialogue_portrait)+
                                    ", text="+std::to_string(result.command.arg1));
                            }
                        } else if (result.status == EventStepStatus::wait && result.command.opcode == 2) {
                            if(!choice_active) { choice_selected_yes=true; ui_detail_page=0; }
                            choice_active = true;
                            dialogue_active = false;
                            dialogue_text = resolve_event_text(event_grid,result.command.arg1,language_bank)
                                .value_or("Make your choice.");
                            dialogue_scroll_lines = 0;
                        } else if (result.status == EventStepStatus::wait && result.command.opcode == 11) {
                            choice_active = false;
                            if (!dialogue_active) {
                                dialogue_portrait = 0;
                                const auto message_index = event_runtime->item_message_string_index();
                                dialogue_text = message_index < language_bank.size()
                                    ? language_bank[message_index]
                                    : (message_index == 3938 ? "Inventory is full." : "No item.");
                                dialogue_scroll_lines = 0;
                                dialogue_active = true;dialogue_reveal.reset();item_message_box = true;
                            }
                        } else if (result.command.opcode == 2 &&
                                   result.status != EventStepStatus::wait) {
                            choice_active = false;
                            dialogue_text.clear();
                            dialogue_scroll_lines = 0;
                        } else if (result.command.opcode == 1 &&
                                   result.status != EventStepStatus::wait) {
                            dialogue_active = false;
                            dialogue_text.clear();
                            dialogue_scroll_lines = 0;
                        } else if (result.command.opcode == 11 &&
                                   result.status != EventStepStatus::wait) {
                            dialogue_active = false;
                            dialogue_text.clear();
                            dialogue_scroll_lines = 0;
                        } else if (result.status == EventStepStatus::actor_command) {
                            if (result.command.opcode >= 15 && result.command.opcode <= 17) {
                                apply_npc_movement_command(result.command, npc_motion);
                            } else if (!hero_input.apply_script_command(result.command.opcode,
                                                                       result.command.arg1)) {
                                event_status_text = "Unsupported actor movement";
                            }
                        } else if (result.status == EventStepStatus::visual_command) {
                            // EMO_HERO: o.c() replaces the hero's whole attached-effect vector first.
                            // NPCs carry no other attached effects here, so EMO_NPC needs only the icon.
                            if (result.command.opcode == 49) combat_world.clear_attached_effects(-1);
                            apply_event_visual_command(event_visuals, result.command,result.remaining_ticks);
                        } else if (result.status == EventStepStatus::audio_command) {
                            // ah BGM_PLAY: 5..7 event music (bw.c), 8 a WAV effect; BGM_STOP ends the
                            // event music. SYSBGM/SOUND never occur in the JAR's events.
                            if (result.command.opcode == 26) {
                                const int id = result.command.arg1;
                                if (id >= 5 && id <= 7) play_music(AudioMixer::event_music, id);
                                else if (id == 8) play_sound(id);
                            } else if (result.command.opcode == 27) audio.stop_music(AudioMixer::event_music);
                        } else if (result.status == EventStepStatus::delegated) {
                            event_status_text = "Unsupported event command " +
                                                std::to_string(result.command.opcode);
                        } else if (result.status == EventStepStatus::map_transition) {
                            if (!result.transition) {
                                event_status_text = "Map transition has no destination";
                            } else if (result.transition->destination_facing > 3) {
                                event_status_text = "Map transition has invalid destination direction";
                            } else {
                                const auto request = *result.transition;
                                const int target_map_id = request.target_map_id;
                                try {
                                    GameMap next_map = load_map(archive, target_map_id);
                                    EventGrid next_event = load_event_grid(
                                        archive, current_zone, target_map_id, next_map);
                                    const auto setup_group = select_setup_group(
                                        next_event, event_runtime->switch_bits());
                                    if (setup_group)
                                        apply_setup_group(next_map, next_event, setup_group->second);

                                    auto next_objects = decode_objects(next_event);
                                    auto next_npcs = decode_npcs(next_event);
                                    auto next_enemies = decode_enemies(next_event);
                                    auto next_npc_motion = make_npc_motion_states(next_event);
                                    HeroInput next_hero(next_map, next_event,
                                                        next_npcs, next_enemies);
                                    next_hero.place_on_tile(request.hero_x, request.hero_y);
                                    next_hero.apply_script_command(6,
                                        static_cast<std::uint8_t>(request.destination_facing + 1));

                                    SpriteTextures next_terrain_textures;
                                    SpriteTextures next_object_textures;
                                    NpcVisualAssets next_npc_assets;
                                    try {
                                        next_terrain_textures = load_terrain_textures(
                                            renderer, archive, next_map);
                                        next_object_textures = load_object_textures(
                                            renderer, archive, next_map, next_event, next_objects);
                                        next_npc_assets = load_npc_textures(
                                            renderer, archive, next_npcs);
                                        load_enemy_textures(
                                            renderer, archive, next_enemies, next_npc_assets);
                                    } catch (...) {
                                        destroy_textures(next_terrain_textures);
                                        destroy_textures(next_object_textures);
                                        destroy_textures(next_npc_assets.textures);
                                        destroy_textures(next_npc_assets.enemy_textures);
                                        throw;
                                    }

                                    destroy_textures(terrain_textures);
                                    destroy_textures(object_textures);
                                    destroy_textures(npc_assets.textures);
                                    destroy_textures(npc_assets.enemy_textures);
                                    terrain_textures = std::move(next_terrain_textures);
                                    object_textures = std::move(next_object_textures);
                                    npc_assets = std::move(next_npc_assets);

                                    map = std::move(next_map);
                                    event_grid = std::move(next_event);
                                    objects = std::move(next_objects);
                                    npcs = std::move(next_npcs);
                                    enemies = std::move(next_enemies);
                                    npc_motion = std::move(next_npc_motion);
                                    enemies_live = false;
                                    rebuild_combat(target_map_id);
                                    world_camera.reset();
                                    hero_input = std::move(next_hero);
                                    current_map_id = target_map_id;
                                    location_started = SDL_GetTicks();
                                    autosave_pending = true;
                                    event_visuals = make_event_visual_state(event_grid);
                                    last_auto_trigger_cell.reset();
                                    startup_log("map transition loaded map " +
                                        std::to_string(current_map_id) + " zone " + current_zone +
                                        " at " + std::to_string(request.hero_x) + "," +
                                        std::to_string(request.hero_y) + " facing-index " +
                                        std::to_string(request.destination_facing));
                                    // ah's opcode 7 calls i() and then c(): the old script
                                    // is discarded. Persist its mutations before releasing it.
                                    persistent_switches = event_runtime->switch_bits();
                                    persistent_quests = event_runtime->quest_bits();
                                    persistent_inventory = event_runtime->inventory();
                                    persistent_money = event_runtime->money();
                                    persistent_vitals = event_runtime->vitals();
                                    persistent_guardians = event_runtime->guardians();
                                    persistent_progress = event_runtime->progress();
                                    event_runtime.reset();
                                    active_actions = nullptr;
                                    active_action_storage.clear();
                                    dialogue_text.clear();
                                    dialogue_scroll_lines = 0;
                                    dialogue_active = choice_active = false;
                                    interaction_requested = menu_requested = false;
                                } catch (const std::exception& error) {
                                    event_status_text = "Map transition failed: " +
                                                        std::string(error.what());
                                    startup_log(event_status_text);
                                }
                            }
                        } else if (result.status == EventStepStatus::ending) {
                            event_visuals.world_hidden = false;
                            persistent_switches = event_runtime->switch_bits();
                            persistent_quests = event_runtime->quest_bits();
                            persistent_inventory = event_runtime->inventory();
                            persistent_money = event_runtime->money();
                            persistent_vitals = event_runtime->vitals();
                            persistent_guardians = event_runtime->guardians();
                            persistent_progress = event_runtime->progress();
                            event_runtime.reset();
                            active_actions = nullptr;
                            active_action_storage.clear();
                            dialogue_text.clear();
                            dialogue_active = choice_active = false;
                            start_ending();
                        } else if (result.status == EventStepStatus::screen_transition) {
                            event_visuals.world_hidden = false;
                            persistent_switches = event_runtime->switch_bits();
                            persistent_quests = event_runtime->quest_bits();
                            persistent_inventory = event_runtime->inventory();
                            persistent_money = event_runtime->money();
                            persistent_vitals = event_runtime->vitals();
                            persistent_guardians = event_runtime->guardians();
                            persistent_progress = event_runtime->progress();
                            const auto screen = result.screen.value_or(EventScreen::shop);
                            service_screen = screen;
                            if (screen != EventScreen::shop) open_service(screen);
                            event_status_text = screen == EventScreen::shop
                                ? "SHOP UI is not implemented. Press B or Esc to return."
                                : screen == EventScreen::refiner
                                    ? "REFINER UI is not implemented. Press B or Esc to return."
                                    : "BLACKSMITH UI is not implemented. Press B or Esc to return.";
                            if (screen != EventScreen::shop) event_status_text.clear();
                            if (screen == EventScreen::shop) {
                                try {
                                    shop_catalog = load_shop_catalog(archive);
                                    if (item_catalog.empty()) item_catalog = load_item_catalog(archive);
                                    shop_selling = false;
                                    shop_active = true;tab_focus = false;
                                    shop_confirming = false; shop_asking = false; shop_notice.clear();
                                    shop_notice.clear();
                                    shop_tab = 0;
                                    shop_selection = 0;
                                    refresh_shop();
                                } catch (const std::exception& error) {
                                    shop_active = false;
                                    event_status_text = "Shop load failed: " + std::string(error.what()) +
                                        ". Press B/Esc to return.";
                                    startup_log(event_status_text);
                                }
                            }
                            event_runtime.reset();
                            active_actions = nullptr;
                            active_action_storage.clear();
                            dialogue_text.clear();
                            dialogue_scroll_lines = 0;
                            dialogue_active = false;
                            choice_active = false;
                            screen_return_active = true;
                            hero_input.sync_world(event_grid, npcs, enemies);
                        } else if (result.status == EventStepStatus::ended) {
                            event_visuals.world_hidden = false;
                            persistent_switches = event_runtime->switch_bits();
                            persistent_quests = event_runtime->quest_bits();
                            persistent_inventory = event_runtime->inventory();
                            persistent_money = event_runtime->money();
                            persistent_vitals = event_runtime->vitals();
                            persistent_guardians = event_runtime->guardians();
                            persistent_progress = event_runtime->progress();
                            event_runtime.reset();
                            active_actions = nullptr;
                            active_action_storage.clear();
                            dialogue_text.clear();
                            dialogue_scroll_lines = 0;
                            dialogue_active = false;
                            choice_active = false;
                        }
                        if (!active_actions || !event_step_continues_frame(result, *active_actions)) break;
                    }
                    if (event_runtime && !dialogue_active && !choice_active && !event_effect_active &&
                        event_status_text.empty()) {
                        hero_input.sync_world(event_grid, npcs, enemies);
                        hero_input.tick();
                        {
                            // The event owns its copies of progress/roster until it ends (ao.f levels the same p).
                            const auto saved_progress = persistent_progress;
                            const auto saved_guardians = persistent_guardians;
                            persistent_progress = event_runtime->progress();
                            persistent_guardians = event_runtime->guardians();
                            auto view = combat_view();
                            persistent_progress = saved_progress;
                            persistent_guardians = saved_guardians;
                            auto vitals = event_runtime->vitals();
                            view.hp = vitals.hp; view.sp = vitals.sp;
                            combat::CombatEvents control_events;
                            combat_world.event_tick(view, control_events);
                            for (const int sound : combat_world.take_sounds()) play_sound(sound);
                            vitals.hp = view.hp; vitals.sp = view.sp;
                            event_runtime->set_vitals(vitals);
                        }
                        const auto moved = tick_npc_movement(
                            npc_motion, map, event_grid, enemies, hero_input.state());
                        for (const auto index : moved) {
                            if (index < npcs.size()) {
                                npcs[index].x = static_cast<std::uint8_t>(npc_motion[index].x_tile);
                                npcs[index].y = static_cast<std::uint8_t>(npc_motion[index].y_tile);
                                npcs[index].partial_x = npc_motion[index].partial_x;
                                npcs[index].partial_y = npc_motion[index].partial_y;
                            }
                        }
                        hero_input.sync_world(event_grid, npcs, enemies);
                    }
                } else if (!event_runtime && !screen_return_active && game_over_until == 0) {
                    hero_input.sync_world(event_grid, npcs, enemies);
                    const auto before = hero_input.state();
                    if (!combat_world.hero_attacking()) {
                        hero_input.tick();
                        if (hero_input.blocked()) {
                            static constexpr int sx[]={0,0,0,-1,1},sy[]={0,-1,1,0,0};
                            const auto h = hero_input.state();
                            combat_world.select_contact(h.x_tile + sx[h.direction], h.y_tile + sy[h.direction]);
                        }
                    }
                    if (deferred_attack && !hero_input.state().partial_x && !hero_input.state().partial_y) {
                        const bool skill = deferred_attack == 2;
                        deferred_attack = 0;
                        hero_input.apply_script_command(5, 0); // stop, then attack from the tile
                        start_attack(skill, true);
                    }
                    const auto after = hero_input.state();
                    if (after.motion_mode == 1) {
                        if (menu_requested) open_menu();
                        else if (interaction_requested) try_start_button_event();
                    }
                    bool auto_check_due = false;
                    // ao.d @926: ah.a(ao) is skipped in state 3, so a sword dash onto a trigger
                    // tile is checked only once the combo has ended.
                    if (after.motion_mode == 1 && !combat_world.hero_attacking()) {
                        const auto cell = static_cast<std::size_t>(after.y_tile) * map.width +
                                          static_cast<std::size_t>(after.x_tile);
                        if (!last_auto_trigger_cell || *last_auto_trigger_cell != cell) {
                            // The original latches automatic tile checks until the hero moves.
                            last_auto_trigger_cell = cell;
                            auto_check_due = true;
                        }
                    }
                    if (!event_runtime && !screen_return_active && before.motion_mode == 2 && after.motion_mode == 1) {
                        static constexpr std::array<int, 5> dx{{0, 0, 0, -1, 1}};
                        static constexpr std::array<int, 5> dy{{0, -1, 1, 0, 0}};
                        if (after.direction >= 1 && after.direction <= 4)
                            try_start_event(after.x_tile + dx[after.direction],
                                            after.y_tile + dy[after.direction],
                                            EventTriggerType::hero_stop_facing_tile);
                    }
                    if (!event_runtime && !screen_return_active && auto_check_due) {
                        try_start_event(after.x_tile, after.y_tile,
                                        EventTriggerType::actor_contact);
                    }
                    // ae.c() (NPC steps) runs only from ah's state 1: outside events an NPC left
                    // walking by MV_N_MOV (m/7/17 action 3) stays where the event ended.
                    if (!event_runtime && !screen_return_active) {
                        std::vector<std::pair<int, int>> npc_tiles;
                        for (std::size_t i = 0; i < npcs.size(); ++i)
                            if (i >= event_grid.npc_visible.size() || event_grid.npc_visible[i]) {
                                npc_tiles.emplace_back(npcs[i].x, npcs[i].y);
                                if (npcs[i].partial_y) npc_tiles.emplace_back(npcs[i].x, npcs[i].y + 1);
                                else if (npcs[i].partial_x) npc_tiles.emplace_back(npcs[i].x + 1, npcs[i].y);
                            }
                        combat_world.set_blocked_tiles(std::move(npc_tiles));
                        auto view = combat_view();
                        if (world_camera.initialized && world_camera.view.scale > 0) {
                            int ow = 0, oh = 0; SDL_GetRendererOutputSize(renderer, &ow, &oh);
                            const auto& v = world_camera.view;
                            combat_world.set_visible_area(-v.left / v.scale, -v.top / v.scale, (ow - v.left) / v.scale, (oh - v.top) / v.scale - 21); // as.b = H-21
                        }
                        combat::CombatEvents combat_events;
                        combat_world.tick(view, combat_events);
                        for (const int sound : combat_world.take_sounds()) play_sound(sound);
                        persistent_vitals.hp = view.hp;
                        persistent_vitals.sp = view.sp;
                        if (combat_events.experience > 0)
                            add_event_experience(persistent_progress, persistent_vitals, persistent_guardians,
                                                 unsigned(combat_events.experience));
                        if (combat_world.hero_dash_offset() > 0) {
                            // ao.b: sword skill dash moves the hero 1-2 tiles forward.
                            const auto hero = hero_input.state();
                            const int dash = combat_world.hero_dash_offset();
                            const int x = hero.x_tile + combat::kDx[std::size_t(hero.direction)] * dash;
                            const int y = hero.y_tile + combat::kDy[std::size_t(hero.direction)] * dash;
                            if (x >= 0 && y >= 0 && x < map.width && y < map.height) {
                                hero_input.place_on_tile(x, y);
                                hero_input.apply_script_command(6, static_cast<std::uint8_t>(hero.direction));
                            }
                        }
                        for (const auto& write : combat_events.overlay_writes)
                            event_grid.overlay[std::size_t(write.y) * map.width + std::size_t(write.x)] =
                                static_cast<std::uint8_t>(write.value);
                        if (combat_events.push_x || combat_events.push_y) {
                            // ao.a(byte,byte): cg's tongue shoves the hero a tile down.
                            hero_input.displace(combat_events.push_x * 16, combat_events.push_y * 16);
                        }
                        if (combat_events.start_trigger) start_trigger_event(combat_events.start_trigger);
                        if (combat_events.hero_died) {
                            audio.stop_all();play_sound(12); // n.e(16): bw.f(), then the game-over jingle
                            // ao.q -> n.a(16): game over, then the title menu (Load/New Game).
                            game_over_until = 1;
                            game_over_frames = 17; // counted down to 16 below, before the first draw
                        }
                        sync_live_enemies();
                    }
                    hero_input.sync_world(event_grid, npcs, enemies);
                }
                if (game_over_until && --game_over_frames <= 0) {
                    game_over_until = 0;
                    combat_world.clear_hero();
                    persistent_vitals.hp = persistent_vitals.max_hp;
                    persistent_vitals.sp = persistent_vitals.max_sp;
                    close_menu();
                    title_menu.open_menu();
                }
                if(dialogue_active) {
                    const auto range=dialogue_page_range();
                    dialogue_reveal.tick(dialogue_text,range.first,range.second,title_menu.instant_speech);
                }
                last_motion_tick = HEROES_LORE_LOW_POWER?advance_logic_tick(last_motion_tick,now,title_menu.tick_ms()):now;
#if HEROES_LORE_LOW_POWER
                ++perf_ticks;
#endif
                ++g_logic_tick;
            }
            VisualUiModel ui;
            ui.hud_elsewhere=HEROES_LORE_3DS;
            if (game_over_until) {
                ui.game_over_chars = (17 - game_over_frames) * 2;
                ui.game_over_text = common_text.size()>32 && !common_text[32].empty()?common_text[32]:std::string("GAME OVER");
            }
            // Port smoothing (20 Hz logic, 60 fps output): fraction of the current tick already shown.
            const double motion_alpha=!title_menu.smooth_motion || title_menu.active || ending || motion_before.map!=current_map_id?1.0:
                std::clamp(double(SDL_GetTicks()-last_motion_tick)/double(title_menu.tick_ms()),0.0,1.0);
            const auto glide=[&](int before,int now_value){
                return std::abs(now_value-before)>16?now_value:int(std::lround(before+(now_value-before)*motion_alpha));};
            HeroState hero_draw=hero_input.state();
            hero_draw.x_pixels=glide(motion_before.hero.x,hero_draw.x_pixels);
            hero_draw.y_pixels=glide(motion_before.hero.y,hero_draw.y_pixels);
            g_npc_draw_offsets.assign(npc_motion.size(),SDL_Point{0,0});
            for(std::size_t i=0;i<npc_motion.size() && i<motion_before.npcs.size();++i)
                g_npc_draw_offsets[i]={glide(motion_before.npcs[i].x,npc_motion[i].x_pixels)-npc_motion[i].x_pixels,
                                       glide(motion_before.npcs[i].y,npc_motion[i].y_pixels)-npc_motion[i].y_pixels};
            g_enemy_draw_offsets.assign(combat_world.enemies().size(),SDL_Point{0,0});
            for(std::size_t i=0;i<combat_world.enemies().size() && i<motion_before.enemies.size();++i) {
                const auto& e=combat_world.enemies()[i];
                g_enemy_draw_offsets[i]={glide(motion_before.enemies[i].x,e.x_px)-e.x_px,glide(motion_before.enemies[i].y,e.y_px)-e.y_px};
            }
            if(!title_menu.active) {
                int w=0,h=0;SDL_GetRendererOutputSize(renderer,&w,&h);
                const auto hero=hero_draw;
                static constexpr int dx[]={0,0,0,-1,1},dy[]={0,-1,1,0,0};
                const int direction=std::clamp(int(hero.direction),0,4);
                const bool detached=event_runtime && !event_runtime->camera.follow;
                // ae.b:Z (maps 13/15): ae.draw uses the unsmoothed n.a/n.b+30, so the view sits 30px higher.
                const bool boss_frame=current_map_id==13 || current_map_id==15;
                const bool smooth=title_menu.smooth_camera && !boss_frame;
                const int lead=smooth && !detached?15:0;
                const int focus_x=detached?glide(motion_before.camera.x,event_runtime->camera.x):hero.x_pixels;
                const int focus_y=(detached?glide(motion_before.camera.y,event_runtime->camera.y):hero.y_pixels)-(boss_frame?30:0);
                const auto target=calculate_world_viewport(w,h,map.width,map.height,
                    focus_x+lead*dx[direction],focus_y+lead*dy[direction],
                    21*std::max(1,std::min(w/320,h/240)));
                if(camera_map!=current_map_id || camera_zone!=current_zone || camera_width!=w || camera_height!=h) world_camera.reset();
                camera_width=w;camera_height=h;
                camera_map=current_map_id;camera_zone=current_zone;
                ui.world_viewport=world_camera.update(target,direction,smooth,camera_tick!=last_motion_tick);
                camera_tick=last_motion_tick;
                // The smooth camera steps once per tick; glide between its last two positions too.
                if(smooth && ui.world_viewport && motion_before.view.scale==ui.world_viewport->scale) {
                    auto& view=*ui.world_viewport;
                    // Camera steps can exceed a tile; only a jump larger than half a screen skips the glide.
                    const auto pan=[&](int before,int now_value){
                        return std::abs(now_value-before)>160*view.scale?now_value:
                            int(std::lround(before+(now_value-before)*motion_alpha));};
                    view.left=pan(motion_before.view.left,view.left);
                    view.top=pan(motion_before.view.top,view.top);
                }
            }
            ui.gold=persistent_money; ui.detail_page=ui_detail_page;
            ui.event_active=bool(event_runtime);
            ui.world_hidden=event_visuals.world_hidden;ui.wide_ui=title_menu.wide_ui;ui.modern_ui=title_menu.modern_ui && title_menu.wide_ui;modern_layout=ui.modern_ui;ui.tab_focus=tab_focus;
            if(const auto* target=combat_world.target_enemy()) {
                const auto& stats=enemy_stats[std::size_t(target->stats)];
                ui.enemy_name=std::size_t(stats.name_text)<language_bank.size()?language_bank[stats.name_text]:"?";
                ui.enemy_level=stats.level;ui.enemy_hp=std::max(0,target->hp);ui.enemy_max_hp=stats.hp;
                ui.enemy_rank=stats.respawn;
            }
            ui.vitals=event_runtime?event_runtime->vitals():persistent_vitals;
            ui.guardians=event_runtime?event_runtime->guardians():persistent_guardians;
            ui.progress=event_runtime?event_runtime->progress():persistent_progress;
            if(shown_level>0 && ui.progress.level>shown_level) {
#if HEROES_LORE_PSP
                psp_diag_stage("level-up",shown_level,ui.progress.level);
#endif
                combat_world.add_level_up_effects();
            }
            shown_level=ui.progress.level;
            { // Port feature: pad rumble when the hero loses HP or a battle/event shake starts (no-op without rumble).
                static int rumble_hp=-1;static bool rumble_shaking=false;
                const auto shake=combat_world.screen_shake();
                const bool shaking=shake.first||shake.second||event_visuals.shake_x||event_visuals.shake_y;
                if(controller && !title_menu.active && title_menu.rumble) {
                    if(rumble_hp>=0 && ui.vitals.hp<rumble_hp) {
                        // Strength follows the share of max HP lost: a scratch is a light tap, a big hit is strong.
                        const float lost=float(rumble_hp-ui.vitals.hp)/float(std::max(1,ui.vitals.max_hp));
                        const float strength=std::clamp(0.06f+lost*2.5f,0.06f,0.7f);
                        const auto level=Uint16(strength*65535.0f);
                        pad_rumble(Uint16(level/2),level,Uint32(70+strength*200.0f),"damage");
                    }
                    else if(shaking && !rumble_shaking) pad_rumble(0x6000,0x4000,180,"shake");
                }
                rumble_hp=title_menu.active?-1:ui.vitals.hp;rumble_shaking=shaking;
#if HEROES_LORE_SWITCH
                switch_rumble.update(SDL_GetTicks());
#endif
            }
            ui.hero_text=hero_text;ui.menu_text=menu_text;ui.common_text=common_text;ui.character_class=new_game.character_class;
            ui.attribute_editing=attribute_editing;ui.attribute_selection=attribute_selection;
            ui.pending_attributes=pending_attributes;
            ui.guardian_choice=guardian_choice;ui.skill_screen=skill_screen;
            ui.item_action=item_action;ui.item_choice=item_choice;ui.item_prompt=item_prompt;
            ui.skill_key_calm=skill_key_calm;ui.skill_selection=skill_selection;
            ui.guardian_skill_text=&guardian_skill_text;ui.language=&language_bank;
            if(latin_font_texture) {ui.latin_font=&latin_font;ui.latin_font_texture=latin_font_texture;}
            ui.localized=archive.has_language_pack();
            ui.buttons=button_style;
            // HUD covers fit two micro letters (guardians) and one (item use).
            if(button_style!=ButtonStyle::keyboard) {
                ui.key_guardian_calm="B";
                ui.key_guardian_wild="Y";
                ui.key_item_use="L";ui.key_item_next="R";
            }
            {
                const auto gear=gear_totals(equipped_items,item_catalog);
                ui.attack=combat::hero_base_attack(ui.progress.attributes[0]+gear.bonus[0])+gear.weapon;
                ui.defense=combat::hero_base_defense(ui.progress.attributes[0]+gear.bonus[0],ui.progress.level)+gear.defense;
            }
            ui.guardian_text=guardian_text;
            ui.startup_heading_font=&startup_heading_font;
            ui.quick_item_type=quick_item_type;
            ui.guardian_channel=combat_world.guardian().state==2;
            if(ui.guardians.equipped<ui.guardians.slots.size())
                if(const auto& active=ui.guardians.slots[ui.guardians.equipped]) {
                    const int skills[2]={active->calm,active->wild};
                    for(std::size_t i=0;i<2;++i) {
                        ui.guardian_cooldown[i]=combat_world.guardian_cooldown(active->id,skills[i]);
                        ui.guardian_cooldown_max[i]=combat::CombatWorld::guardian_cooldown_full(active->id,skills[i]);
                    }
                }
            const auto& visible_bag=event_runtime?event_runtime->inventory():persistent_inventory;
            if(const auto slot=quick_item_slot(visible_bag,quick_item_type)) ui.quick_item_count=visible_bag[*slot].count;
            if(!world_notice.empty() && static_cast<Sint32>(world_notice_until-SDL_GetTicks())>0)
                ui.notice=world_notice;
            ui.location_name=location_name_for(current_map_id);
            const auto minimap_view=[&](bool lit) {
                MinimapView view;view.width=map.width;view.height=map.height;
                view.hero_x=hero_input.state().x_tile;view.hero_y=hero_input.state().y_tile;view.hero_lit=lit;
                view.cells.resize(std::size_t(map.width)*map.height);
                for(std::size_t i=0;i<view.cells.size();++i) {
                    const int c=i<event_grid.overlay.size()?int(static_cast<std::int8_t>(event_grid.overlay[i])):0;
                    auto colour=kMinimapColours[map.kind*2+(c<0?1:0)];
                    if(c!=0 && c!=-128 && any_event_condition_met(event_grid,std::abs(c),persistent_switches,persistent_quests,
                           [&](std::uint8_t a,std::uint8_t b,std::uint8_t n){return has_inventory_requirement(persistent_inventory,a,b,n);}))
                        colour=map.kind==6?0xFF3FBF:0x3F7FFF;
                    view.cells[i]=colour;
                }
                return view;
            };
            if(minimap_open && map.kind<=14) ui.minimap=minimap_view(minimap_lit);
#if !HEROES_LORE_CONSOLE
            if(std::getenv("HEROES_LORE_SNAPSHOT_BOTTOM") && map.kind<=14) ui.bottom_minimap=minimap_view(true);
#endif
#if HEROES_LORE_3DS
            if(map.kind<=14) { // rebuilt when the hero changes tile or map, and twice a second for switches
                static MinimapView bottom;static int bottom_map=-1,bottom_x=-1,bottom_y=-1;static Uint32 bottom_built=0;
                const auto tile=hero_input.state();
                if(bottom_map!=current_map_id || bottom_x!=tile.x_tile || bottom_y!=tile.y_tile || SDL_GetTicks()-bottom_built>500) {
                    bottom=minimap_view(true);bottom_map=current_map_id;bottom_x=tile.x_tile;bottom_y=tile.y_tile;bottom_built=SDL_GetTicks();
                }
                ui.bottom_minimap=bottom;
            }
#endif
            // ae.draw steps the banner once per painted tick (bs.run: one i() and one repaint).
            ui.location_step=int(std::min<Uint32>(10,(SDL_GetTicks()-location_started)/title_menu.tick_ms()));
#if !HEROES_LORE_CONSOLE
            if(snapshot_screen) {
                const std::string screen=snapshot_screen;
                if(screen=="location") ui.location_step=3;
                else ui.location_step=10;
            }
#endif
            const auto present_item = [&](const ShopItem* item,int type,int id,int count) {
                VisualItem row; row.type=type; row.id=id; row.count=count;
                row.stackable=type>=0 && is_stackable_item(static_cast<std::uint8_t>(type));
                if(!item) {row.name="Unknown item"; return row;}
                row.name=shop_text(item->name_key);row.description=shop_text(item->description_key);
                // '|' marks text formatting in the language bank; it is never drawn.
                row.description.erase(std::remove(row.description.begin(),row.description.end(),'|'),row.description.end());
                row.stats=item_stats_text(*item);row.price=item->price;
                if(const auto info=equipment_info(*item)) {row.power=int(info->power);row.weapon=item->type<=2;}
                const int dest=equipment_slot(item->type);
                if(dest>=0 && equipped_items[dest]) {
                    const auto& gear=*equipped_items[dest];
                    if(const auto* old=find_catalog_item(item_catalog,gear.item_type,gear.item_id)) {
                        row.comparison=tr("Equipped: ","Надето - ")+shop_text(old->name_key);
                        const auto a=equipment_info(*item),b=equipment_info(*old);
                        if(a && b) { const int delta=int(a->power)-int(b->power);
                            row.compare=delta>0?1:delta<0?-1:0;
                            row.comparison+=tr("\nBase change: ","\nИзменение ")+std::string(delta>=0?"+":"")+std::to_string(delta); }
                    }
                }
                return row;
            };
            // cb.a(g,x,y,ad): refining e/d, red option name, "Power+3" bonus lines before the description.
            const auto present_stack = [&](const InventoryStack& stack) {
                const auto* item=find_catalog_item(item_catalog,stack.item_type,stack.item_id);
                auto row=present_item(item,stack.item_type,stack.item_id,stack.count);
                row.identified=stack.identified;row.refine=stack.refine;
                if(item) if(const auto info=equipment_info(*item)) row.refine_max=info->secondary;
                const int option=stack.item_type<=3?item_option(stack,item):-1;
                if(option>=0 && std::size_t(option)<attribute_text.size()) row.option_name=attribute_text[std::size_t(option)];
                std::string bonus;
                for(std::size_t i=0;i<4;++i) if(stack.bonus[i]>0 && 9+i<hero_text.size())
                    bonus+=hero_text[9+i]+"+"+std::to_string(stack.bonus[i])+"\n";
                row.description=bonus+row.description;
                if(stack.item_type>=0 && std::size_t(stack.item_type)<item_type_text.size()) row.type_name=item_type_text[std::size_t(stack.item_type)];
                return row;
            };
            if(ending) {
                ending_view=EndingView{};
                ending_view.hero=new_game.character_class;
                ending_view.credits=ending->phase==EndingSequence::Phase::credits;
                ending_view.fade=ending->fade_level();
                ending_view.first=ending->first;ending_view.counter=ending->counter;
                for(int i=ending->first;i<ending->end && i<int(ending->story.size());++i) ending_view.page.push_back(ending->story[std::size_t(i)]);
                for(const auto& line:ending->rolling) ending_view.rolling.emplace_back(line.text,line.y);
                ui.screen=VisualScreen::ending;ui.ending=&ending_view;ui.world_hidden=true;
            } else if(title_menu.active) {
                ui.screen=VisualScreen::title_menu;ui.selection=std::size_t(title_menu.selection);
                ui.save_available=title_menu.continue_available;ui.text=title_menu.message;ui.world_hidden=true;
                ui.startup=title_menu;ui.help_text=help_text;ui.about_text=about_text;ui.language=&language_bank;
            } else if(shop_active) {
                ui.screen=VisualScreen::shop;ui.title=shop_selling?tr("TRADING POST / SELL","ЛАВКА - ПРОДАЖА"):tr("TRADING POST / BUY","ЛАВКА - ПОКУПКА");
                ui.selection=shop_selection;ui.category=shop_tab;ui.selling=shop_selling;
                ui.notice=shop_notice;ui.confirming=shop_confirming;ui.quantity=shop_quantity;
                ui.shop_asking=shop_asking;ui.shop_question=shop_question;ui.shop_text=&shop_ui_text;
                ui.common_text=common_text;ui.menu_text=menu_text;
                for(const auto& entry:shop_rows) {
                    const auto* item=entry.item;
                    const int count=shop_selling?persistent_inventory[entry.slot].count:1;
                    auto row=present_item(item,item?int(item->type):-1,item?int(item->id):-1,count);
                    if(shop_selling && item) {
                        row.price=static_cast<std::uint32_t>(shop_sale_value(*item,1));
                        if(!can_sell_item_type(item->type)) row.description=tr("This item cannot be sold.","Этот предмет нельзя продать.")+"\n"+row.description;
                    }
                    ui.items.push_back(std::move(row));
                }
                if(shop_confirming && shop_selection<ui.items.size()) {
                    const auto& row=ui.items[shop_selection];const auto* item=shop_rows[shop_selection].item;
                    ui.modal_title=(shop_selling?bank_or(3912,"Sell"):bank_or(3914,"Buy"))+" "+row.name;
                    const auto cost=shop_selling?shop_sale_value(*item,shop_quantity):std::int64_t(item->price)*shop_quantity;
                    const std::string g=bank_or(3948,"G");
                    ui.modal_text=tr("Quantity: ","Количество ")+std::to_string(shop_quantity)+tr("   Left/Right: change\n","   влево/вправо\n")+
                        (shop_selling?tr("Receive: ","Получите "):tr("Total: ","Итого "))+std::to_string(cost)+" "+g+"\n"+
                        tr("Gold: ","Золото ")+std::to_string(persistent_money)+" "+g;
                }
            } else if(menu_active) {
                ui.screen=VisualScreen::game_menu;
                ui.menu_tab=menu_page;ui.status_page=menu_selection;ui.vitals=persistent_vitals;
                ui.selection=menu_page==5?std::size_t(save_selection):menu_page==2?equipment_selection:menu_page==3?guardian_selection:menu_page==4?(recipes_view?recipe_selection:quest_selection):bag_selection;
                ui.quests=build_quest_journal(new_game.character_class,quest_text,persistent_quests);
                ui.quest_reading=quest_reading;
                ui.recipes_view=recipes_view;
                if(recipes_view) {
                    // Icons and carried counts match the recipe's words to item names, case-folded
                    // (the lines write "root", the catalogue "Root"; Cyrillic folds too).
                    const auto fold=[](const std::string& text) {
                        std::string out=text;
                        for(std::size_t i=0;i<out.size();++i) {
                            auto& c=reinterpret_cast<unsigned char&>(out[i]);
                            if(c<0x80) {c=static_cast<unsigned char>(std::tolower(c));continue;}
                            if(c!=0xD0 || i+1>=out.size()) continue;
                            auto& n=reinterpret_cast<unsigned char&>(out[i+1]);
                            if(n>=0x90 && n<=0x9F) n=static_cast<unsigned char>(n+0x20);
                            else if(n>=0xA0 && n<=0xAF) {c=0xD1;n=static_cast<unsigned char>(n-0x20);}
                            else if(n==0x81) {c=0xD1;n=0x91;}
                            ++i;
                        }
                        return out;
                    };
                    for(const auto& entry:known_recipes()) {
                        VisualRecipe recipe;recipe.result=entry.result;
                        for(const auto& item:item_catalog)
                            if(fold(shop_text(item.name_key))==fold(entry.result)) {recipe.icon_type=int(item.type);break;}
                        for(const auto& part:entry.parts) {
                            int count=0;
                            for(const auto& stack:persistent_inventory)
                                if(const auto* item=find_catalog_item(item_catalog,stack.item_type,stack.item_id);
                                   item && fold(shop_text(item->name_key))==fold(part)) count+=stack.count;
                            recipe.parts.push_back({part,count});
                        }
                        ui.recipes.push_back(std::move(recipe));
                    }
                }
                ui.confirming=equipment_confirming || unequip_confirming || save_confirming || attribute_confirming;ui.notice=equipment_notice;
                if(attribute_confirming) {ui.modal_title=menu_text.size()>33?menu_text[33]:"";ui.modal_text.clear();}
                ui.save_available=save_exists(new_game.character_class);
                if(save_confirming) {
                    // Quit asks bh "Are You Sure?" (3904); saving itself needs no question (d).
                    ui.modal_title=language_bank.size()>3904?language_bank[3904]:"Are you sure?";ui.modal_text.clear();
                }
                for(const auto& stack:persistent_inventory) ui.items.push_back(present_stack(stack));
                const char* labels[]={"Weapon","Shield","Armor","Headgear","Accessory"};
                for(std::size_t slot=0;slot<equipped_items.size();++slot) {
                    if(equipped_items[slot]) ui.equipped.push_back(present_stack(*equipped_items[slot]));
                    else {VisualItem empty;empty.name=std::string(labels[slot])+": empty";ui.equipped.push_back(empty);}
                }
                if(unequip_confirming && equipment_selection<ui.equipped.size()) {
                    ui.modal_title=tr("Unequip ","Снять ")+ui.equipped[equipment_selection].name+"?";ui.modal_text.clear();
                }
                if(equipment_confirming && bag_selection<ui.items.size()) {
                    ui.modal_title="EQUIP "+ui.items[bag_selection].name;
                    ui.modal_text=ui.items[bag_selection].comparison+"\nPrevious equipment returns to your bag.";
                }
            } else if(choice_active) {
                ui.screen=VisualScreen::choice;ui.title=tr("YOUR CHOICE","ВАШ ВЫБОР");ui.text=dialogue_text;ui.choice_yes=choice_selected_yes;
                ui.language=&language_bank;ui.common_text=common_text;
            } else if(service) {
                const auto& s=*service;const auto inv=service_inventory();
                service_view=ServiceView{};auto& v=service_view;
                v.blacksmith=s.kind==ServiceKind::blacksmith;v.page=s.page;v.menu=s.menu;v.cursor=s.cursor;
                v.text=v.blacksmith?&blacksmith_text:&refiner_text;
                v.picking=s.picking;v.pick_sel=s.pick_sel;v.pick_title=s.pick_title;
                for(int ref:s.picks) if(const auto* stack=service_item(inv,ref)) v.picks.push_back(present_stack(*stack));
                for(int code:s.message) {
                    const auto& text=code>=0?(std::size_t(code)<v.text->size()?(*v.text)[std::size_t(code)]:std::string()):bank_or(std::size_t(-code),"");
                    if(text.empty()) continue;
                    if(!v.message.empty()) v.message+="\n";
                    v.message+=text;
                }
                v.question=s.question;v.busy=s.busy!=0;v.mix_confirm=s.mix_confirm;
                if(const auto* stack=service_item(inv,s.item)) {v.item=present_stack(*stack);v.has_item=true;}
                if(const auto* stack=service_item(inv,s.orb)) {
                    v.orb=present_stack(*stack);v.has_orb=true;
                    if(std::size_t(stack->item_id)<attribute_text.size()) v.option_name=attribute_text[std::size_t(stack->item_id)];
                }
                for(std::size_t i=0;i<3;++i) if(const auto* stack=service_item(inv,s.mix[i])) {v.mix[i]=present_stack(*stack);v.has_mix[i]=true;}
                v.cost=s.cost;v.need=s.need;
                ui.screen=VisualScreen::service;ui.service=&service_view;ui.gold=persistent_money;
                ui.common_text=common_text;ui.menu_text=menu_text;ui.hero_text=hero_text;
            } else if(!event_status_text.empty()) {
                if(screen_return_active && service_screen && *service_screen!=EventScreen::shop) {
                    ui.screen=VisualScreen::service;ui.title=*service_screen==EventScreen::refiner?tr("REFINER","РЕФАЙНЕР"):tr("BLACKSMITH","КУЗНЕЦ");
                    ui.text=tr("This service is not ported yet.","Эта услуга пока не перенесена.");
                } else {ui.screen=VisualScreen::notice;ui.title=tr("EVENT","СОБЫТИЕ");ui.text=event_status_text;}
            } else if(dialogue_active) {
                ui.screen=VisualScreen::dialogue;ui.text=dialogue_text;ui.portrait=dialogue_portrait;ui.scroll_lines=dialogue_scroll_lines;
                ui.item_box=item_message_box;ui.language=&language_bank;
                ui.dialogue_visible_end=title_menu.instant_speech?std::numeric_limits<std::size_t>::max():dialogue_reveal.end;
#if !HEROES_LORE_CONSOLE
                if(snapshot_screen && std::string(snapshot_screen)!="dialogue-gradual" &&
                   std::string(snapshot_screen)!="dialogue-chain-gradual")
                    ui.dialogue_visible_end=std::numeric_limits<std::size_t>::max();
#endif
            }
#if HEROES_LORE_LOW_POWER
            const Uint32 render_started=SDL_GetTicks();
#endif
#if HEROES_LORE_PSP
            const bool trace_hit_frame=std::any_of(combat_world.popups().begin(),combat_world.popups().end(),[](const auto& popup){return popup.age==0;});
            if(trace_hit_frame) psp_diag_stage("hit-draw-begin",int(combat_world.popups().size()),int(combat_world.battle_effects().size()));
#endif
            render(renderer, map, event_grid, objects, npcs, enemies, npc_motion,
                   hero_draw, show_collision,
                   terrain_textures, object_textures, npc_assets, hero_assets, event_visuals,
                   ui, dialogue_font_texture, dialogue_font, dialogue_textures,
                   combat_world, combat_textures, guardian_fx, guardian_skill_text,death_effects);
#if HEROES_LORE_3DS
            perf_render_ms+=SDL_GetTicks()-render_started;++perf_frames;
            if(SDL_GetTicks()-perf_started>=5000) {
                startup_log("3DS perf: frames="+std::to_string(perf_frames)+", elapsed_ms="+std::to_string(SDL_GetTicks()-perf_started)+", render_ms="+std::to_string(perf_render_ms)+
                    ", logic_ticks="+std::to_string(perf_ticks)+" (tick_ms="+std::to_string(title_menu.tick_ms())+")"+
                    ", field/ui/top/bottom_ms="+std::to_string(g_3ds_phase_ms[0])+"/"+std::to_string(g_3ds_phase_ms[1])+"/"+
                    std::to_string(g_3ds_phase_ms[2])+"/"+std::to_string(g_3ds_phase_ms[3])+", bottom_redraws="+std::to_string(g_3ds_bottom_redraws)+" (draw/read/present_ms="+std::to_string(g_3ds_bottom_ms[0])+"/"+
                    std::to_string(g_3ds_bottom_ms[1])+"/"+std::to_string(g_3ds_bottom_ms[2])+")"+
                    ", screen="+std::to_string(int(ui.screen)));
                g_3ds_bottom_redraws=0;for(auto& value:g_3ds_bottom_ms) value=0;
                perf_started=SDL_GetTicks();perf_frames=perf_render_ms=perf_ticks=0;
                for(auto& value:g_3ds_phase_ms) value=0;
            }
#endif
#if HEROES_LORE_PSP
            if(trace_hit_frame) psp_diag_stage("hit-draw-done");
            perf_render_ms+=SDL_GetTicks()-render_started;++perf_frames;
            if(SDL_GetTicks()-perf_started>=5000) {
                startup_log("PSP perf: frames="+std::to_string(perf_frames)+", elapsed_ms="+std::to_string(SDL_GetTicks()-perf_started)+", render_ms="+std::to_string(perf_render_ms)+", audio_peak_ms="+std::to_string(audio_peak_ms.exchange(0)));
                perf_started=SDL_GetTicks();perf_frames=perf_render_ms=0;
            }
#endif
            // Battle shake persists for the game tick, independent of render FPS.
            if (first_frame && !startup_loading) {
                startup_log("first frame submitted");
                first_frame = false;
#if !HEROES_LORE_CONSOLE
                if(snapshot_screen) if(const char* output=std::getenv("HEROES_LORE_SNAPSHOT_OUT")) {
                    const auto all_nearest = [&](const SpriteTextures& textures) {
                        for(const auto& entry:textures) {SDL_ScaleMode mode{};
                            if(SDL_GetTextureScaleMode(entry.second,&mode)!=0 || mode!=SDL_ScaleModeNearest) return false;}
                        return true;
                    };
                    SDL_ScaleMode font_mode{};
                    const bool nearest=SDL_GetTextureScaleMode(dialogue_font_texture,&font_mode)==0 &&
                        font_mode==SDL_ScaleModeNearest && all_nearest(dialogue_textures) &&
                        all_nearest(terrain_textures) && all_nearest(object_textures) && all_nearest(hero_assets.textures);
                    const std::string state_path=std::string(output)+".state.json";
                    auto* file=std::fopen(state_path.c_str(),"w");
                    if(!file) throw std::runtime_error("Unable to save UI snapshot state");
                    std::fprintf(file,"{\"gold\":%lld,\"bag_slots\":%zu,\"weapon_id\":%d,\"bag_first_id\":%d,"
                        "\"equipment_confirming\":%s,\"shop_confirming\":%s,\"portrait\":%d,\"nearest\":%s,"
                        "\"dialogue_active\":%s,\"event_cursor\":%d,\"npc_marker\":%d,\"visible_text_end\":%zu,"
                        "\"title_active\":%s,\"map_id\":%d,\"hero_x\":%d,\"hero_y\":%d}\n",
                        static_cast<long long>(persistent_money),persistent_inventory.size(),
                        equipped_items[0]?int(equipped_items[0]->item_id):-1,
                        persistent_inventory.empty()?-1:int(persistent_inventory[0].item_id),
                        equipment_confirming?"true":"false",shop_confirming?"true":"false",dialogue_portrait,nearest?"true":"false",
                        dialogue_active?"true":"false",event_runtime?int(event_runtime->cursor()):-1,
                        event_visuals.npc_emotes.empty()?0:int(event_visuals.npc_emotes[0]),ui.dialogue_visible_end,
                        title_menu.active?"true":"false",current_map_id,hero_input.state().x_tile,hero_input.state().y_tile);
                    std::fclose(file);running=false;
                }
#endif
            }
            if(startup_loading) {
                startup_loading=false;
                try {
                    boot_progress=[&](int progress) {
                        VisualUiModel loading;loading.screen=VisualScreen::title_menu;
                        loading.startup=title_menu;loading.startup.page=TitlePage::loading;loading.loading_progress=progress;
                        int w=0,h=0;SDL_GetRendererOutputSize(renderer,&w,&h);
                        draw_visual_ui(renderer,loading,dialogue_textures,dialogue_font_texture,dialogue_font,w,h,{});
                        SDL_RenderPresent(renderer);
#if HEROES_LORE_3DS
                        present_3ds_frame();
#ifdef HEROES_LORE_3DS_GPU
                        hl3d::end_frame();
#endif
#endif
                    };
                    begin_new_game(title_menu.character_class,title_menu.chosen_guardians);
                    boot_progress={};title_menu.active=false;location_started=SDL_GetTicks();
                } catch(const std::exception& error) {boot_progress={};title_menu.page=TitlePage::menu;title_menu.message=error.what();}
            }
#if HEROES_LORE_PSP
            psp_pace_frame_30fps();
#elif HEROES_LORE_3DS
            { // 30 FPS cap; the software renderer has no vsync
                static Uint32 next_frame=0;const Uint32 tick=SDL_GetTicks();
                if(next_frame && int(next_frame-tick)>0) SDL_Delay(next_frame-tick);
                const Uint32 after=SDL_GetTicks();next_frame=(next_frame && int(after-next_frame)<34)?next_frame+33:after+33;
            }
#else
            if (!renderer_has_vsync) SDL_Delay(8);
#endif
        }
        startup_log("render loop exited normally");
        audio_device.stop(); // before SDL_Quit
#ifdef HEROES_LORE_3DS_GPU
        startup_log("shutdown: audio stopped");
        if(hl3d::closing()) {
            // Closed from HOME: the screens and GPU already belong to the system, and SDL's video
            // teardown (gfxExit) waits for screen events that no longer come. The process ends anyway.
            startup_log("shutdown: HOME close, video teardown skipped");
            return 0;
        }
#endif
        if (controller) SDL_GameControllerClose(controller);
        destroy_textures(terrain_textures);
        destroy_textures(dialogue_textures);
        destroy_textures(object_textures);
        destroy_textures(npc_assets.textures);
        destroy_textures(npc_assets.enemy_textures);
        destroy_textures(hero_assets.textures);
        if (dialogue_font_texture) SDL_DestroyTexture(dialogue_font_texture);
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
#if HEROES_LORE_3DS
#ifdef HEROES_LORE_3DS_GPU
        hl3d::shutdown();
#endif
        startup_log("shutdown: audio closed, SDL_Quit");
        SDL_Quit();
#elif HEROES_LORE_CONSOLE
        SDL_QuitSubSystem(SDL_INIT_VIDEO); // not SDL_Quit: closing audio would join its thread (see AudioDevice)
#else
        SDL_Quit();
#endif
    } catch (const std::exception& error) {
        startup_log("ERROR: " + std::string(error.what()));
        return 1;
    }
    return 0;
}
