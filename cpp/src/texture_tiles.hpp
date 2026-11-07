#pragma once
#include <SDL.h>

namespace hl_texture {
SDL_Texture* create(SDL_Renderer*, Uint32, int, int, int);
int update(SDL_Texture*, const SDL_Rect*, const void*, int);
int query(SDL_Texture*, Uint32*, int*, int*, int*);
void destroy(SDL_Texture*);
int color(SDL_Texture*, Uint8, Uint8, Uint8);
int blend(SDL_Texture*, SDL_BlendMode);
int scale(SDL_Texture*, SDL_ScaleMode);
int copy(SDL_Renderer*, SDL_Texture*, const SDL_Rect*, const SDL_Rect*);
int copy_ex(SDL_Renderer*, SDL_Texture*, const SDL_Rect*, const SDL_Rect*, double, const SDL_Point*, SDL_RendererFlip);
}
// Only the PSP frontend uses logical textures split into hardware-sized pieces.
#if defined(HEROES_LORE_PSP_BUILD) && !defined(HL_TEXTURE_IMPLEMENTATION)
#define SDL_CreateTexture hl_texture::create
#define SDL_UpdateTexture hl_texture::update
#define SDL_QueryTexture hl_texture::query
#define SDL_DestroyTexture hl_texture::destroy
#define SDL_SetTextureColorMod hl_texture::color
#define SDL_SetTextureBlendMode hl_texture::blend
#define SDL_SetTextureScaleMode hl_texture::scale
#define SDL_RenderCopy hl_texture::copy
#define SDL_RenderCopyEx hl_texture::copy_ex
#endif
