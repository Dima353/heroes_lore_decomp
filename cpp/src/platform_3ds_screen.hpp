#pragma once
// Nintendo 3DS only: presenting the top and bottom screens, the audio core and HOME-menu hook.
#include <3ds.h>
#include <SDL.h>
#include <functional>
#include "visual_ui.hpp"

constexpr Uint8 kTouchTap=0x7A; // padding1 of controller events made from bottom-screen taps
extern SDL_Surface* g_3ds_frame;       // the top screen picture of the software renderer
extern bool g_3ds_audio_core;          // SDL's audio thread got 80% of the system core
extern SDL_AudioDeviceID g_3ds_audio;
extern aptHookCookie g_3ds_apt_cookie;
void apt_hook_3ds(APT_HookType hook,void*);
extern Uint32 g_3ds_bottom_redraws;    // full bottom-screen redraws (perf line)
extern Uint32 g_3ds_bottom_ms[3];      // of those: draw, read-back, compare + present
extern Uint32 g_3ds_input_serial;      // bumped by every button, key or touch event
extern Uint32 g_3ds_phase_ms[4];       // field, interface, top screen copy, bottom screen
void present_3ds_frame();
void draw_3ds_bottom(SDL_Renderer* renderer,const VisualUiModel& ui,const UiTextureMap& textures,
                     SDL_Texture* font_texture,const BitmapFont& font,bool hud,bool menu,
                     const std::function<void(SDL_Rect)>& hero_preview);
