#pragma once
// Nintendo 3DS GPU renderer (citro2d) behind the SDL 2 render calls the game uses.
// With HEROES_LORE_3DS_GPU the SDL_* names below are redirected here: draws are recorded per screen
// and replayed on the GPU at hl3d::end_frame(), the top screen once per eye for stereoscopic 3D
// (each draw shifted by its layer depth and the 3D slider). Other builds see only HL3D_DEPTH.
#include <SDL.h>

#if defined(HEROES_LORE_3DS_GPU)
namespace hl3d {
bool init();                 // after SDL's video init (gfxInit), before any texture
void shutdown();
bool closing();               // HOME closed the app: the GPU and screens belong to the system now
void end_frame();            // replay both screens, swap
void set_depth(float depth); // 0 at the screen plane, 1 far behind; < 0: ground plane tilted by screen y
void target_bottom(bool bottom);
SDL_Texture* create_texture(SDL_Renderer*, Uint32 format, int access, int w, int h);
int update_texture(SDL_Texture*, const SDL_Rect*, const void* pixels, int pitch);
int query_texture(SDL_Texture*, Uint32* format, int* access, int* w, int* h);
void destroy_texture(SDL_Texture*);
int set_texture_color_mod(SDL_Texture*, Uint8 r, Uint8 g, Uint8 b);
int set_texture_blend_mode(SDL_Texture*, SDL_BlendMode);
int set_texture_scale_mode(SDL_Texture*, SDL_ScaleMode);
int get_texture_scale_mode(SDL_Texture*, SDL_ScaleMode*);
int render_copy(SDL_Renderer*, SDL_Texture*, const SDL_Rect* src, const SDL_Rect* dst);
int render_copy_ex(SDL_Renderer*, SDL_Texture*, const SDL_Rect* src, const SDL_Rect* dst, double angle,
                   const SDL_Point* center, SDL_RendererFlip flip);
int set_draw_color(SDL_Renderer*, Uint8 r, Uint8 g, Uint8 b, Uint8 a);
int set_draw_blend_mode(SDL_Renderer*, SDL_BlendMode);
int fill_rect(SDL_Renderer*, const SDL_Rect*);
int draw_rect(SDL_Renderer*, const SDL_Rect*);
int draw_line(SDL_Renderer*, int x1, int y1, int x2, int y2);
int clear(SDL_Renderer*);
int set_clip_rect(SDL_Renderer*, const SDL_Rect*);
void get_clip_rect(SDL_Renderer*, SDL_Rect*);
SDL_bool is_clip_enabled(SDL_Renderer*);
void get_viewport(SDL_Renderer*, SDL_Rect*);
int set_scale(SDL_Renderer*, float, float);
int get_output_size(SDL_Renderer*, int* w, int* h);
void present(SDL_Renderer*);
int read_pixels(SDL_Renderer*, const SDL_Rect*, Uint32, void*, int);
int flush(SDL_Renderer*);
int set_render_target(SDL_Renderer*, SDL_Texture*);
int get_renderer_info(SDL_Renderer*, SDL_RendererInfo*);
} // namespace hl3d
#define HL3D_DEPTH(d) hl3d::set_depth(d)

#if !defined(HL3D_IMPLEMENTATION)
#define SDL_CreateTexture hl3d::create_texture
#define SDL_UpdateTexture hl3d::update_texture
#define SDL_QueryTexture hl3d::query_texture
#define SDL_DestroyTexture hl3d::destroy_texture
#define SDL_SetTextureColorMod hl3d::set_texture_color_mod
#define SDL_SetTextureBlendMode hl3d::set_texture_blend_mode
#define SDL_SetTextureScaleMode hl3d::set_texture_scale_mode
#define SDL_GetTextureScaleMode hl3d::get_texture_scale_mode
#define SDL_RenderCopy hl3d::render_copy
#define SDL_RenderCopyEx hl3d::render_copy_ex
#define SDL_SetRenderDrawColor hl3d::set_draw_color
#define SDL_SetRenderDrawBlendMode hl3d::set_draw_blend_mode
#define SDL_RenderFillRect hl3d::fill_rect
#define SDL_RenderDrawRect hl3d::draw_rect
#define SDL_RenderDrawLine hl3d::draw_line
#define SDL_RenderClear hl3d::clear
#define SDL_RenderSetClipRect hl3d::set_clip_rect
#define SDL_RenderGetClipRect hl3d::get_clip_rect
#define SDL_RenderIsClipEnabled hl3d::is_clip_enabled
#define SDL_RenderGetViewport hl3d::get_viewport
#define SDL_RenderSetScale hl3d::set_scale
#define SDL_GetRendererOutputSize hl3d::get_output_size
#define SDL_RenderPresent hl3d::present
#define SDL_RenderReadPixels hl3d::read_pixels
#define SDL_RenderFlush hl3d::flush
#define SDL_SetRenderTarget hl3d::set_render_target
#define SDL_GetRendererInfo hl3d::get_renderer_info
#endif
#else
#define HL3D_DEPTH(d) ((void)0)
#endif
