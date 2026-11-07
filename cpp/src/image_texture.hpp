#pragma once
#include "platform.hpp"
#include "gpu_3ds.hpp" // the 3DS GPU path redirects SDL_UpdateTexture below
#include <SDL.h>
#include <vector>

// Texture format for the game's RGBA byte images. The 3DS software renderer draws in ARGB8888, the
// format SDL 2 has fast blend / colour-modulate / scale blitters for (ABGR fell to SDL_Blit_Slow on
// every tinted glyph), so images are swizzled once when uploaded.
#if HEROES_LORE_3DS && !defined(HEROES_LORE_3DS_GPU)
constexpr Uint32 kImageTextureFormat = SDL_PIXELFORMAT_ARGB8888;
#else
constexpr Uint32 kImageTextureFormat = SDL_PIXELFORMAT_RGBA32;
#endif

inline int upload_image_texture(SDL_Texture* texture, const void* rgba, int width, int height) {
#if HEROES_LORE_3DS && !defined(HEROES_LORE_3DS_GPU)
    std::vector<Uint32> argb(std::size_t(width) * std::size_t(height));
    if (SDL_ConvertPixels(width, height, SDL_PIXELFORMAT_RGBA32, rgba, width * 4,
                          SDL_PIXELFORMAT_ARGB8888, argb.data(), width * 4) != 0) return -1;
    return SDL_UpdateTexture(texture, nullptr, argb.data(), width * 4);
#else
    (void)height;
    return SDL_UpdateTexture(texture, nullptr, rgba, width * 4);
#endif
}
