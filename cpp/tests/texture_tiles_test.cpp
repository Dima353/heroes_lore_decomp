#include "texture_tiles.hpp"
#include <cstdio>
#include <vector>
#include <stdexcept>
static void check(bool ok) {if(!ok) throw std::runtime_error(SDL_GetError());}
int main(int, char**) {
    check(SDL_Init(0)==0);
    auto* surface=SDL_CreateRGBSurfaceWithFormat(0,1100,850,32,SDL_PIXELFORMAT_RGBA32);
    auto* renderer=SDL_CreateSoftwareRenderer(surface);check(renderer);
    std::vector<Uint32> pixels(1031*777);
    for(int y=0;y<777;++y) for(int x=0;x<1031;++x)
        pixels[y*1031+x]=SDL_MapRGBA(surface->format,x%256,y%256,(x+y)%256,255);
    auto* reference=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,1031,777);
    auto* tiled=hl_texture::create(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,1031,777);check(tiled && reference);
    check(SDL_UpdateTexture(reference,nullptr,pixels.data(),1031*4)==0);
    check(hl_texture::update(tiled,nullptr,pixels.data(),1031*4)==0);
    int w=0,h=0;check(hl_texture::query(tiled,nullptr,nullptr,&w,&h)==0 && w==1031 && h==777);
    check(hl_texture::color(tiled,190,220,160)==0);SDL_SetTextureColorMod(reference,190,220,160);
    SDL_Rect patch{500,500,30,30};std::vector<Uint32> patch_pixels(900,SDL_MapRGBA(surface->format,255,0,200,255));
    check(hl_texture::update(tiled,&patch,patch_pixels.data(),30*4)==0);
    check(SDL_UpdateTexture(reference,&patch,patch_pixels.data(),30*4)==0);
    std::vector<Uint32> expected(1100*850),actual(expected.size());
    for(int mode=0;mode<2;++mode) for(int f=0;f<4;++f) {
        SDL_Rect src{490,490,60,40},dst=mode?SDL_Rect{3,5,120,80}:SDL_Rect{0,0,1031,777};
        auto flip=static_cast<SDL_RendererFlip>(f);
        SDL_SetRenderDrawColor(renderer,0,0,0,255);SDL_RenderClear(renderer);
        check(SDL_RenderCopyEx(renderer,reference,mode?&src:nullptr,&dst,0,nullptr,flip)==0);
        check(SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,expected.data(),1100*4)==0);
        SDL_RenderClear(renderer);
        check(hl_texture::copy_ex(renderer,tiled,mode?&src:nullptr,&dst,0,nullptr,flip)==0);
        check(SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,actual.data(),1100*4)==0);
        if(expected!=actual) {std::fprintf(stderr,"Mismatch mode=%d flip=%d\n",mode,f);return 1;}
    }
    hl_texture::destroy(tiled);SDL_DestroyTexture(reference);
    // A fresh create after destruction must not retain the old logical dimensions.
    tiled=hl_texture::create(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,8,8);
    check(hl_texture::query(tiled,nullptr,nullptr,&w,&h)==0 && w==8 && h==8);
    hl_texture::destroy(tiled);
    // Real JAR shapes: char/lvup frame 3, menu strip 19, and thin sprite layers.
    // SDL PSP swizzles textures when either axis >=16; its 16-byte x 8-row
    // swizzler requires BOTH storage axes to contain complete blocks.
    for(const auto shape: {SDL_Point{1,20},SDL_Point{93,1},SDL_Point{26,4},SDL_Point{17,3},SDL_Point{18,4},SDL_Point{21,4}}) {
        tiled=hl_texture::create(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,shape.x,shape.y);check(tiled);
        int physical_w=0,physical_h=0;
        check(SDL_QueryTexture(tiled,nullptr,nullptr,&physical_w,&physical_h)==0);
        const auto pow2=[](int n){int value=1;while(value<n)value*=2;return value;};
        if(physical_w>=16 || physical_h>=16) {
            if((pow2(physical_w)*4)%16 || pow2(physical_h)%8)
                throw std::runtime_error("SDL PSP swizzle storage is not a complete 16-byte x 8-row block");
        }
        check(hl_texture::query(tiled,nullptr,nullptr,&w,&h)==0 && w==shape.x && h==shape.y);
        std::vector<Uint32> tiny(shape.x*shape.y);
        for(std::size_t i=0;i<tiny.size();++i) tiny[i]=SDL_MapRGBA(surface->format,i%256,(i*7)%256,250,255);
        reference=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,w,h);check(reference);
        check(SDL_UpdateTexture(reference,nullptr,tiny.data(),w*4)==0);
        check(hl_texture::update(tiled,nullptr,tiny.data(),w*4)==0);
        for(int f=0;f<4;++f) {
            SDL_Rect dst{7,11,w*2,h*2};auto flip=static_cast<SDL_RendererFlip>(f);
            SDL_RenderClear(renderer);SDL_RenderCopyEx(renderer,reference,nullptr,&dst,0,nullptr,flip);
            SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,expected.data(),1100*4);
            SDL_RenderClear(renderer);check(hl_texture::copy_ex(renderer,tiled,nullptr,&dst,0,nullptr,flip)==0);
            SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,actual.data(),1100*4);
            if(expected!=actual) throw std::runtime_error("Padded thin texture changed visible pixels");
        }
        SDL_DestroyTexture(reference);hl_texture::destroy(tiled);
    }
    SDL_DestroyRenderer(renderer);SDL_FreeSurface(surface);SDL_Quit();
    std::puts("PASS: tiled texture full image, cross-boundary crop, 4 flips, color, partial update, destroy/recreate and thin PSP swizzle blocks");
}
