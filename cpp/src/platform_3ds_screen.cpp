#include "platform.hpp"
#if HEROES_LORE_3DS
#include <SDL.h>
#include "gpu_3ds.hpp"
#include "platform_3ds_screen.hpp"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

// SDL's N3DS window surface is RGBA8888, a format without fast blitters in SDL 2 (every sprite went
// through SDL_Blit_Slow). The game draws into an ARGB8888 surface (see image_texture.hpp) and
// this rotates it into the top screen's framebuffer (240x400, column-major, RGBA8 packed).
SDL_Surface* g_3ds_frame=nullptr;
// libctru's default split caps the normal heap at 24 MB and gives the rest (~25 MB on Old 3DS) to the
// linear heap, which only the framebuffers and DSP buffers use (~2 MB here). The embedded SoundFont
// alone needs ~13 MB of normal heap, so keep 10 MB linear and the rest (~40 MB) for malloc.
#ifdef HEROES_LORE_3DS_GPU
extern "C" { u32 __ctru_linear_heap_size = 16u*1024u*1024u; } // + GPU textures and citro2d vertices
#else
extern "C" { u32 __ctru_linear_heap_size = 10u*1024u*1024u; }
#endif
bool g_3ds_audio_core=false; // SDL's audio thread got 80% of the system core
// HOME menu and sleep: SDL's audio thread runs on the system core, which the HOME menu needs.
// While suspended the audio pauses and the core limit drops (as the sm64 3DS port does).
SDL_AudioDeviceID g_3ds_audio=0;
aptHookCookie g_3ds_apt_cookie;
void apt_hook_3ds(APT_HookType hook,void*) {
    if(!g_3ds_audio) return;
    if(hook==APTHOOK_ONSUSPEND || hook==APTHOOK_ONSLEEP || hook==APTHOOK_ONEXIT) {
        SDL_PauseAudioDevice(g_3ds_audio,1);
        APT_SetAppCpuTimeLimit(30);
    } else if(hook==APTHOOK_ONRESTORE || hook==APTHOOK_ONWAKEUP) {
        APT_SetAppCpuTimeLimit(80);
        SDL_PauseAudioDevice(g_3ds_audio,0);
    }
}
Uint32 g_3ds_bottom_redraws=0; // full bottom-screen redraws (perf line)
Uint32 g_3ds_bottom_ms[3]{};    // of those: draw, read-back, compare + present
Uint32 g_3ds_input_serial=0;    // bumped by every button, key or touch event
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
void present_3ds_frame() {
    if(g_3ds_frame) present_3ds_screen(GFX_TOP,g_3ds_frame->pixels,g_3ds_frame->w,g_3ds_frame->h,g_3ds_frame->pitch);
}
// Bottom screen (320x240): the HUD strip, drawn through a render target of the same software renderer
// (SDL textures belong to one renderer) and read back every frame.
void draw_3ds_bottom(SDL_Renderer* renderer,const VisualUiModel& ui,const UiTextureMap& textures,
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
void present_3ds_frame() {}
void draw_3ds_bottom(SDL_Renderer* renderer,const VisualUiModel& ui,const UiTextureMap& textures,
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
Uint32 g_3ds_phase_ms[4]{}; // field, interface, top screen copy, bottom screen
#endif
