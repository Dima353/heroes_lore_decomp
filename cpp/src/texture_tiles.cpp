#define HL_TEXTURE_IMPLEMENTATION
#include "texture_tiles.hpp"
#include <algorithm>
#include <unordered_map>
#include <vector>

namespace hl_texture {
namespace {
struct Tile { SDL_Texture* texture; SDL_Rect bounds; };
struct Image { int w,h,access; Uint32 format; std::vector<Tile> tiles; };
std::unordered_map<SDL_Texture*, Image> images;
template<class F> int apply(SDL_Texture* texture, F f) {
    const auto it=images.find(texture);
    if(it==images.end()) return f(texture);
    for(const auto& tile:it->second.tiles) if(f(tile.texture)!=0) return -1;
    return 0;
}
}
SDL_Texture* create(SDL_Renderer* renderer, Uint32 format, int access, int w, int h) {
    SDL_RendererInfo info{};
    if(SDL_GetRendererInfo(renderer,&info)!=0) return nullptr;
    const int limit_w=info.max_texture_width>0?std::min(512,info.max_texture_width):512;
    const int limit_h=info.max_texture_height>0?std::min(512,info.max_texture_height):512;
    if(w<=0 || h<=0) return SDL_CreateTexture(renderer,format,access,w,h);
    // SDL PSP 2.32.10 swizzles if EITHER dimension >=16, but its swizzle
    // kernel requires 16-byte rows and 8-row blocks. Pad the actual RGBA
    // allocation while retaining logical dimensions and source rectangles.
    const int min_w=4, min_h=8;
    if(w<=limit_w && h<=limit_h && w>=min_w && h>=min_h)
        return SDL_CreateTexture(renderer,format,access,w,h);
    if(access!=SDL_TEXTUREACCESS_STATIC) {SDL_SetError("Tiled textures require static access");return nullptr;}
    Image image{w,h,access,format,{}};
    for(int y=0;y<h;y+=limit_h) for(int x=0;x<w;x+=limit_w) {
        SDL_Rect bounds{x,y,std::min(limit_w,w-x),std::min(limit_h,h-y)};
        auto* tile=SDL_CreateTexture(renderer,format,access,std::max(min_w,bounds.w),std::max(min_h,bounds.h));
        if(!tile) {for(const auto& t:image.tiles) SDL_DestroyTexture(t.texture);return nullptr;}
        image.tiles.push_back({tile,bounds});
    }
    auto* handle=image.tiles.front().texture;
    images.emplace(handle,std::move(image));
    return handle;
}
int query(SDL_Texture* texture, Uint32* format, int* access, int* w, int* h) {
    const auto it=images.find(texture);
    if(it==images.end()) return SDL_QueryTexture(texture,format,access,w,h);
    const auto& image=it->second;
    if(format) *format=image.format;
    if(access) *access=image.access;
    if(w) *w=image.w;
    if(h) *h=image.h;
    return 0;
}
int update(SDL_Texture* texture, const SDL_Rect* rect, const void* pixels, int pitch) {
    const auto it=images.find(texture);
    if(it==images.end()) return SDL_UpdateTexture(texture,rect,pixels,pitch);
    const auto& image=it->second;
    const SDL_Rect area=rect?*rect:SDL_Rect{0,0,image.w,image.h};
    const int bytes=SDL_BYTESPERPIXEL(image.format);
    if(!pixels || bytes!=4 || pitch<area.w*bytes) return SDL_SetError("Invalid tiled RGBA update");
    for(const auto& tile:image.tiles) {
        SDL_Rect overlap{};
        if(!SDL_IntersectRect(&area,&tile.bounds,&overlap)) continue;
        const SDL_Rect local{overlap.x-tile.bounds.x,overlap.y-tile.bounds.y,overlap.w,overlap.h};
        const auto* data=static_cast<const Uint8*>(pixels)+(overlap.y-area.y)*pitch+(overlap.x-area.x)*bytes;
        if(SDL_UpdateTexture(tile.texture,&local,data,pitch)!=0) return -1;
    }
    return 0;
}
void destroy(SDL_Texture* texture) {
    const auto it=images.find(texture);
    if(it==images.end()) {SDL_DestroyTexture(texture);return;}
    for(const auto& tile:it->second.tiles) SDL_DestroyTexture(tile.texture);
    images.erase(it);
}
int color(SDL_Texture* texture, Uint8 r, Uint8 g, Uint8 b) {return apply(texture,[&](SDL_Texture* t){return SDL_SetTextureColorMod(t,r,g,b);});}
int blend(SDL_Texture* texture, SDL_BlendMode mode) {return apply(texture,[&](SDL_Texture* t){return SDL_SetTextureBlendMode(t,mode);});}
int scale(SDL_Texture* texture, SDL_ScaleMode mode) {return apply(texture,[&](SDL_Texture* t){return SDL_SetTextureScaleMode(t,mode);});}
int copy_ex(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* src, const SDL_Rect* dst,
            double angle, const SDL_Point* center, SDL_RendererFlip flip) {
    const auto it=images.find(texture);
    if(it==images.end()) {
        if(angle==0 && flip==SDL_FLIP_NONE) return SDL_RenderCopy(renderer,texture,src,dst);
        return SDL_RenderCopyEx(renderer,texture,src,dst,angle,center,flip);
    }
    if(angle!=0) return SDL_SetError("Rotated tiled textures are unsupported");
    const auto& image=it->second;
    const SDL_Rect source=src?*src:SDL_Rect{0,0,image.w,image.h};
    SDL_Rect dest{};
    if(dst) dest=*dst;
    else {SDL_RenderGetViewport(renderer,&dest);dest.x=dest.y=0;}
    if(source.w<=0 || source.h<=0 || dest.w<=0 || dest.h<=0) return 0;
    for(const auto& tile:image.tiles) {
        SDL_Rect overlap{};
        if(!SDL_IntersectRect(&source,&tile.bounds,&overlap)) continue;
        const int x=(flip&SDL_FLIP_HORIZONTAL)?source.x+source.w-overlap.x-overlap.w:overlap.x-source.x;
        const int y=(flip&SDL_FLIP_VERTICAL)?source.y+source.h-overlap.y-overlap.h:overlap.y-source.y;
        // Shared integer edges avoid gaps when scaling a crop across tile boundaries.
        const int x0=x*dest.w/source.w, x1=(x+overlap.w)*dest.w/source.w;
        const int y0=y*dest.h/source.h, y1=(y+overlap.h)*dest.h/source.h;
        const SDL_Rect target{dest.x+x0,dest.y+y0,x1-x0,y1-y0};
        const SDL_Rect local{overlap.x-tile.bounds.x,overlap.y-tile.bounds.y,overlap.w,overlap.h};
        const int result=flip==SDL_FLIP_NONE?
            SDL_RenderCopy(renderer,tile.texture,&local,&target):
            SDL_RenderCopyEx(renderer,tile.texture,&local,&target,0,nullptr,flip);
        if(result!=0) return -1;
    }
    return 0;
}
int copy(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* src, const SDL_Rect* dst) {
    return copy_ex(renderer,texture,src,dst,0,nullptr,SDL_FLIP_NONE);
}
}
