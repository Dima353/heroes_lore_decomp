#include "texture_tiles.hpp"
#include "visual_ui.hpp"
#include "language_pack.hpp"
#include "platform.hpp"
#include "image_texture.hpp"
#include "gpu_3ds.hpp"
#include "viewport.hpp"
#include "world_presentation.hpp"
#include <algorithm>
#include <cctype>
#include <array>
#include <cstdlib>

namespace {
// cb: 0x3f1f3f, 0x5f3f3f, 0xdfbf7f; original menu/choice palette.
constexpr SDL_Color ink{63,31,63,255}, panel{95,63,63,255}, raised{159,127,127,255};
constexpr SDL_Color gold{255,159,0,255}, white{239,223,255,255}, muted{223,191,127,255};
struct Painter {
    SDL_Renderer* renderer;
    const UiTextureMap& textures;
    SDL_Texture* font_texture;
    const BitmapFont& font;
    int scale;
    void rect(SDL_Rect r, SDL_Color c) const {
        SDL_SetRenderDrawColor(renderer,c.r,c.g,c.b,c.a); SDL_RenderFillRect(renderer,&r);
    }
    void outline(SDL_Rect r, SDL_Color c, int width=1) const {
        for (int i=0;i<width;++i) {
            SDL_SetRenderDrawColor(renderer,c.r,c.g,c.b,c.a); SDL_RenderDrawRect(renderer,&r);
            ++r.x; ++r.y; r.w-=2; r.h-=2;
        }
    }
    void frame(SDL_Rect r, bool selected=false, SDL_Color fill=panel) const {
        rect({r.x+3*scale,r.y+3*scale,r.w,r.h},{0,0,0,150});
        rect(r,fill); outline(r,selected ? gold : muted,scale);
        SDL_Rect inside{r.x+2*scale,r.y+2*scale,r.w-4*scale,r.h-4*scale};
        outline(inside,ink,scale);
        for (int i=0;i<4;++i) {
            const auto found=textures.find(i<2 ? 7 : 8);
            if (found==textures.end()) continue;
            int w=0,h=0; SDL_QueryTexture(found->second,nullptr,nullptr,&w,&h);
            const bool right=i&1;
            SDL_Rect d{right ? r.x+r.w-(w+2)*scale : r.x+2*scale,
                i<2 ? r.y+2*scale : r.y+r.h-(h+2)*scale,w*scale,h*scale};
            SDL_RenderCopyEx(renderer,found->second,nullptr,&d,0,nullptr,right ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
        }
    }
    void text(const std::string& value, SDL_Rect bounds, SDL_Color color=white, int scroll=0,
              std::size_t visible_end=std::numeric_limits<std::size_t>::max()) const {
        if (!font_texture || bounds.w<=0 || bounds.h<=0) return;
        SDL_Rect old{}; SDL_RenderGetClipRect(renderer,&old);
        const bool clipped=SDL_RenderIsClipEnabled(renderer)==SDL_TRUE;
        SDL_Rect effective=bounds;
        if(clipped && !SDL_IntersectRect(&old,&bounds,&effective)) return;
        SDL_RenderSetClipRect(renderer,&effective);
        SDL_SetTextureColorMod(font_texture,color.r,color.g,color.b);
        const auto glyphs=layout_bitmap_text(font,value,bounds.x,bounds.y-scroll*int(font.height)*scale,bounds.w,scale);
        for (const auto& glyph:glyphs) {
            if(glyph.text_end>visible_end) continue;
            if (!glyph.source_width) continue;
            SDL_Rect src{int(glyph.source_x),0,int(glyph.source_width),font.height};
            SDL_Rect dst{glyph.x,glyph.y,int(glyph.source_width)*scale,int(font.height)*scale};
            SDL_RenderCopy(renderer,font_texture,&src,&dst);
        }
        SDL_SetTextureColorMod(font_texture,255,255,255);
        SDL_RenderSetClipRect(renderer,clipped ? &old : nullptr);
    }
    void line(std::string value,SDL_Rect bounds,SDL_Color color=white) const {
        const auto width = [&](const std::string& text_value) {
            int right=0;
            for(const auto& g:layout_bitmap_text(font,text_value,0,0,100000,scale))
                right=std::max(right,g.x+int(g.source_width)*scale);
            return right;
        };
        if(width(value)>bounds.w) {
            do {
                if(value.empty()) break;
                std::size_t at=value.size()-1;
                while(at>0 && (static_cast<unsigned char>(value[at])&0xc0)==0x80) --at;
                value.resize(at);
            } while(width(value+"...")>bounds.w);
            value+="...";
        }
        bounds.h=int(font.height)*scale;
        text(value,bounds,color);
    }
    void icon(int type,SDL_Rect box) const {
        rect(box,raised); outline(box,{65,80,99,255},scale);
        if(type<0) { rect({box.x+box.w/3,box.y+box.h/2,box.w/3,std::max(1,scale)},{67,82,100,255}); return; }
        const auto found=textures.find(200+type);
        if (found==textures.end()) {
            text("?",{box.x+box.w/2-3*scale,box.y+box.h/2-5*scale,8*scale,14*scale},muted); return;
        }
        int w=0,h=0; SDL_QueryTexture(found->second,nullptr,nullptr,&w,&h);
        const int s=std::max(1,std::min((box.w-8*scale)/std::max(1,w),(box.h-8*scale)/std::max(1,h)));
        SDL_Rect d{box.x+(box.w-w*s)/2,box.y+(box.h-h*s)/2,w*s,h*s};
        SDL_RenderCopy(renderer,found->second,nullptr,&d);
    }
    void button(SDL_Rect r,const std::string& label,bool selected=false) const {
        rect(r,selected ? SDL_Color{66,55,36,255} : raised); outline(r,selected ? gold : SDL_Color{68,87,106,255},scale);
        if (selected) rect({r.x,r.y,3*scale,r.h},gold);
        text(label,{r.x+10*scale,r.y+(r.h-int(font.height)*scale)/2,r.w-16*scale,int(font.height)*scale},selected?gold:white);
    }
    void paragraph(const std::string& value,SDL_Rect bounds,int page,SDL_Color color=white) const {
        const auto glyphs=layout_bitmap_text(font,value,0,0,bounds.w,scale);
        int lines=1;
        for (const auto& g:glyphs) lines=std::max(lines,g.y/(int(font.height)*scale)+1);
        const int visible=std::max(1,(bounds.h-16*scale)/(int(font.height)*scale));
        const int pages=std::max(1,(lines+visible-1)/visible), current=page%pages;
        text(value,{bounds.x,bounds.y,bounds.w,visible*int(font.height)*scale},color,current*visible);
        if (pages>1) text(std::to_string(current+1)+"/"+std::to_string(pages),
            {bounds.x,bounds.y+bounds.h-13*scale,bounds.w,13*scale},gold);
    }
};
// Port-only text: English, or Russian re-encoded for the Russian font (which has no ':' glyph).
std::string tr_text(const VisualUiModel& model,const char* en,const char* ru) {
    return model.localized?std::string(ru):std::string(en);
}
std::string bank_text(const VisualUiModel& model,int index,const char* fallback) {
    return model.language && std::size_t(index)<model.language->size()?(*model.language)[std::size_t(index)]:std::string(fallback);
}
// Set per draw_visual_ui call from the model.
static ButtonStyle g_buttons=ButtonStyle::keyboard;
// The keyboard / pad name of a Switch face button; PlayStation buttons are drawn as shapes instead.
static std::string platform_key(const std::string& key) {
    if(g_buttons!=ButtonStyle::keyboard) return key;
    return key=="A"?"Enter":key=="B"?"Esc":key=="Y"?"Tab":key;
}
static int playstation_shape(const std::string& key) {
    if(g_buttons!=ButtonStyle::playstation) return -1;
    return key=="A"?0:key=="B"?1:key=="X"?2:key=="Y"?3:-1;
}
// Draws (when draw) the key cap and returns its width: "Key:" in the Latin font, or a 7x7 ✕○□△ shape.
void draw_micro_text(SDL_Renderer* renderer,const std::string& text,int x,int y,int scale,SDL_Color color);
int micro_text_width(const std::string& text);
static int key_cap(const Painter& keys,const std::string& key,int x,int y,int h,SDL_Color color,bool draw) {
    if(const int shape=playstation_shape(key);shape>=0) {
        static const char* const bits[4]={
            "1000001010001000101000001000001010001000101000001","0011100010001010000011000001100000101000100011100",
            "1111111100000110000011000001100000110000011111111","0001000001010000101000100010010001010000011111111"};
        static const SDL_Color colours[4]={{124,178,232,255},{255,102,102,255},{255,105,248,255},{64,226,160,255}};
        const int k=keys.scale,top=y+std::max(0,(std::min(h,int(keys.font.height)*k)-7*k)/2);
        if(draw) {
            SDL_SetRenderDrawColor(keys.renderer,colours[shape].r,colours[shape].g,colours[shape].b,255);
            for(int i=0;i<49;++i) if(bits[shape][i]=='1') {
                SDL_Rect pixel{x+(i%7)*k,top+(i/7)*k,k,k};SDL_RenderFillRect(keys.renderer,&pixel);
            }
        }
        return 8*k;
    }
    if(g_buttons==ButtonStyle::keyboard) {
        // Keyboard keys as small caps in the port's 3x5 letters: a Russian JAR font has no Latin glyphs.
        std::string name;
        for(std::size_t start=0;start<=key.size();) {
            const auto end=std::min(key.find('/',start),key.size());
            if(!name.empty()) name+='/';
            for(char c:platform_key(key.substr(start,end-start))) name+=char(std::toupper(static_cast<unsigned char>(c)));
            start=end+1;
        }
        const int k=keys.scale,w=(micro_text_width(name)+4)*k;
        const int top=y+std::max(0,(std::min(h,int(keys.font.height)*k)-9*k)/2);
        if(draw) {
            keys.rect({x,top,w,9*k},{31,15,31,255});keys.outline({x,top,w,9*k},color,k);
            draw_micro_text(keys.renderer,name,x+2*k,top+2*k,k,color);
        }
        return w;
    }
    const std::string text=platform_key(key)+":";
    int w=0;
    for(const auto& g:layout_bitmap_text(keys.font,text,0,0,1000,keys.scale))
        w=std::max(w,g.x+int(g.source_width)*keys.scale);
    if(draw) keys.text(text,{x,y,w+keys.scale,h},color);
    return w;
}
// Control hints: the key cap, then its word in the game font.
// plate draws a dark backing so the hint stays readable over the HUD; right aligns to r's right edge.
int key_hints_width(const Painter& words,const Painter& keys,const std::vector<std::pair<std::string,std::string>>& hints) {
    int x=0;
    for(const auto& hint:hints) {
        if(!hint.first.empty()) x+=key_cap(keys,hint.first,0,0,0,{},false)+3*words.scale;
        int word_w=0;
        for(const auto& g:layout_bitmap_text(words.font,hint.second,0,0,1000,words.scale))
            word_w=std::max(word_w,g.x+int(g.source_width)*words.scale);
        x+=word_w+10*words.scale;
    }
    return std::max(0,x-10*words.scale);
}
void draw_key_hints(const Painter& words,const Painter& keys,SDL_Rect r,SDL_Color color,
                    const std::vector<std::pair<std::string,std::string>>& hints,bool plate=false,bool right=false) {
    const int width=key_hints_width(words,keys,hints);
    int x=right?r.x+r.w-width:r.x;
    if(plate) {
        const int pad=3*words.scale;
        const SDL_Rect back{x-pad,r.y-pad,width+2*pad,r.h+pad};
        words.rect(back,{16,8,16,215});
        words.outline(back,muted,std::max(1,words.scale/2));
    }
    for(const auto& hint:hints) {
        if(!hint.first.empty()) x+=key_cap(keys,hint.first,x,r.y,r.h,color,true)+3*words.scale;
        int word_w=0;
        for(const auto& g:layout_bitmap_text(words.font,hint.second,0,0,1000,words.scale))
            word_w=std::max(word_w,g.x+int(g.source_width)*words.scale);
        words.text(hint.second,{x,r.y,word_w+words.scale,r.h},color);
        x+=word_w+10*words.scale;
    }
}
// 3x5 pixel letters for button names drawn over the keypad digits of img/uifrm.
void draw_micro_text(SDL_Renderer* renderer,const std::string& text,int x,int y,int scale,SDL_Color color) {
    static const std::pair<char,const char*> glyphs[]={
        {'0',"111101101101111"},{'1',"010110010010111"},{'2',"111001111100111"},{'3',"111001011001111"},
        {'9',"111101111001111"},{'Z',"111001010100111"},{'L',"100100100100111"},{'R',"110101110101101"},
        {'Q',"010101101110011"},{'E',"111100110100111"},{'Y',"101101010010010"},{'X',"101101010101101"},
        {'A',"010101111101101"},{'B',"110101110101110"},{'T',"111010010010010"},{'N',"101111111101101"},
        {'S',"111100111001111"},{'C',"111100100100111"},{'/',"001001010100100"}};
    SDL_SetRenderDrawColor(renderer,color.r,color.g,color.b,color.a);
    for(char c:text) {
        const char* bits=nullptr;
        for(const auto& glyph:glyphs) if(glyph.first==c) bits=glyph.second;
        if(bits) for(int i=0;i<15;++i) if(bits[i]=='1') {
            SDL_Rect pixel{x+(i%3)*scale,y+(i/3)*scale,scale,scale};SDL_RenderFillRect(renderer,&pixel);
        }
        x+=4*scale;
    }
}
int micro_text_width(const std::string& text) {return text.empty()?0:int(text.size())*4-1;}
// cb.a(g,x,y,w,h,false): 0x3F1F3F outline, 0x9F7F3F top/left and 0x3F3F3F bottom/right bevel (lines only).
void cb_frame(const Painter& p,int x,int y,int w,int h) {
    const int k=p.scale;
    const auto R=[&](int rx,int ry,int rw,int rh,SDL_Color c){if(rw>0 && rh>0) p.rect({(x+rx)*k,(y+ry)*k,rw*k,rh*k},c);};
    const SDL_Color outer{63,31,63,255},light{159,127,63,255},dark{63,63,63,255};
    R(1,0,w-2,1,outer);R(0,1,1,h-2,outer);R(w-1,1,1,h-2,outer);R(1,h-1,w-2,1,outer);
    R(1,1,w-3,1,light);R(1,1,1,h-3,light);
    R(w-2,1,1,h-3,dark);R(1,h-2,w-2,1,dark);
}

std::string category_name(const VisualUiModel& model,int type) {
    switch(type) {
        case 0: return tr_text(model,"Sword","Меч"); case 1: return tr_text(model,"Axe","Топор");
        case 2: return tr_text(model,"Staff","Посох"); case 3: return tr_text(model,"Shield","Щит");
        case 4: return tr_text(model,"Accessory","Украшение"); case 5: return tr_text(model,"Armor","Доспех");
        case 6: return tr_text(model,"Headgear","Шлем"); default: return tr_text(model,"Item","Предмет"); }
}
void detail_card(const Painter& p,SDL_Rect r,const VisualUiModel& model) {
    const int s=p.scale; p.frame(r);
    if (model.items.empty() || model.selection>=model.items.size()) {
        p.text(tr_text(model,"NO ITEM SELECTED","ПРЕДМЕТ НЕ ВЫБРАН"),{r.x+12*s,r.y+16*s,r.w-24*s,30*s},muted); return;
    }
    const auto& item=model.items[model.selection];
    p.icon(item.type,{r.x+12*s,r.y+14*s,44*s,44*s});
    p.text(category_name(model,item.type),{r.x+66*s,r.y+18*s,r.w-78*s,20*s},muted);
    p.text(tr_text(model,"x","шт. ")+std::to_string(item.count),{r.x+66*s,r.y+38*s,r.w-78*s,20*s},gold);
    p.text(item.name,{r.x+12*s,r.y+70*s,r.w-24*s,32*s},gold);
    p.text(item.stats,{r.x+12*s,r.y+110*s,r.w-24*s,30*s});
    p.text(item.comparison,{r.x+12*s,r.y+142*s,r.w-24*s,32*s},muted);
    p.paragraph(item.description,{r.x+12*s,r.y+180*s,r.w-24*s,std::max(20*s,r.h-192*s)},model.detail_page);
}
}

static std::string language_text(const VisualUiModel& model,int index,const char* fallback) {
    return model.language && std::size_t(index)<model.language->size()?(*model.language)[std::size_t(index)]:std::string(fallback);
}

// Port: menus, shops and services sit on a blurred, darkened copy of the field instead of black.
// The field is static while they are open, so the frame is read back once, blurred on a
// small CPU buffer and stretched with linear filtering.
static SDL_Texture* menu_backdrop=nullptr;
static void capture_menu_backdrop(SDL_Renderer* renderer,int output_w,int output_h) {
    if(menu_backdrop) return;
    std::vector<Uint32> frame(std::size_t(output_w)*std::size_t(output_h));
    if(SDL_RenderReadPixels(renderer,nullptr,SDL_PIXELFORMAT_RGBA32,frame.data(),output_w*4)!=0) return;
    // Low-power software rendering: full size, so drawing it each frame is a plain copy, not a stretch.
    const int step=HEROES_LORE_LOW_POWER?1:std::max(1,output_w/160),w=std::max(1,output_w/step),h=std::max(1,output_h/step);
    std::vector<int> rgb(std::size_t(w)*std::size_t(h)*3);
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) { // box-average each step x step cell
        int sum[3]{};
        for(int yy=0;yy<step;++yy) for(int xx=0;xx<step;++xx) {
            const Uint8* c=reinterpret_cast<const Uint8*>(&frame[std::size_t(y*step+yy)*std::size_t(output_w)+std::size_t(x*step+xx)]);
            sum[0]+=c[0];sum[1]+=c[1];sum[2]+=c[2];
        }
        for(int k=0;k<3;++k) rgb[(std::size_t(y)*std::size_t(w)+std::size_t(x))*3+std::size_t(k)]=sum[k]/(step*step);
    }
    std::vector<int> tmp(rgb.size());
    const int radius=2;
    for(int pass=0;pass<2;++pass) {
        for(int y=0;y<h;++y) for(int x=0;x<w;++x) for(int k=0;k<3;++k) {
            int sum=0,n=0;
            for(int d=-radius;d<=radius;++d) {const int xx=std::clamp(x+d,0,w-1);
                sum+=rgb[(std::size_t(y)*std::size_t(w)+std::size_t(xx))*3+std::size_t(k)];++n;}
            tmp[(std::size_t(y)*std::size_t(w)+std::size_t(x))*3+std::size_t(k)]=sum/n;
        }
        for(int y=0;y<h;++y) for(int x=0;x<w;++x) for(int k=0;k<3;++k) {
            int sum=0,n=0;
            for(int d=-radius;d<=radius;++d) {const int yy=std::clamp(y+d,0,h-1);
                sum+=tmp[(std::size_t(yy)*std::size_t(w)+std::size_t(x))*3+std::size_t(k)];++n;}
            rgb[(std::size_t(y)*std::size_t(w)+std::size_t(x))*3+std::size_t(k)]=sum/n;
        }
    }
    std::vector<Uint8> pixels(std::size_t(w)*std::size_t(h)*4);
    for(std::size_t i=0;i<std::size_t(w)*std::size_t(h);++i) {
        for(std::size_t k=0;k<3;++k) pixels[i*4+k]=Uint8(rgb[i*3+k]*55/100); // darken so the frame stands out
        pixels[i*4+3]=255;
    }
    menu_backdrop=SDL_CreateTexture(renderer,kImageTextureFormat,SDL_TEXTUREACCESS_STATIC,w,h);
    if(!menu_backdrop) return;
    SDL_SetTextureScaleMode(menu_backdrop,SDL_ScaleModeLinear);
    upload_image_texture(menu_backdrop,pixels.data(),w,h);
}
static void release_menu_backdrop() {
    if(menu_backdrop) SDL_DestroyTexture(menu_backdrop);
    menu_backdrop=nullptr;
}

// The HUD strip (as.a bottom uifrm pieces), in its own pass so the 3DS can show it on the bottom screen.
void draw_world_hud(SDL_Renderer* renderer,const VisualUiModel& model,const UiTextureMap& textures,
    SDL_Texture* font_texture,const BitmapFont& font,int output_w,int output_h,int scale) {
    const int hs=scale>0?scale:std::max(1,std::min(output_w/320,output_h/240));
    Painter hud{renderer,textures,font_texture,font,hs};
    const int base=output_h-36*hs;
    const auto sprite=[&](int key,int x,int y) {
        const auto found=textures.find(key);if(found==textures.end()) return;
        int iw=0,ih=0;SDL_QueryTexture(found->second,nullptr,nullptr,&iw,&ih);
        SDL_Rect dst{x,y,iw*hs,ih*hs};SDL_RenderCopy(renderer,found->second,nullptr,&dst);
    };
    // r.c variant 0 (img/glb 5): 5px digit cells advancing 4px, right-anchored at x.
    const auto hud_digits=[&](int value,int right,int y) {
        const std::string text=std::to_string(std::max(0,value));
        const int x=right-int(text.size())*4*hs;
        const auto strip=textures.find(0x50005);
        if(strip==textures.end()) {hud.line(text,{x,y,int(text.size()*6+1)*hs,8*hs},white);return;}
        for(std::size_t i=0;i<text.size();++i) {
            SDL_Rect src{(text[i]-'0')*5,0,5,7},dst{x+int(i)*4*hs,y,5*hs,7*hs};
            SDL_RenderCopy(renderer,strip->second,&src,&dst);
        }
    };
    sprite(1,0,base+12*hs);sprite(1,22*hs,base+12*hs);
    sprite(2,23*hs,base+23*hs);sprite(3,44*hs,base+12*hs);
    for(int x=49*hs;x<output_w-25*hs;x+=6*hs) sprite(4,x,base+14*hs);
    sprite(0,0,base+9*hs);
    sprite(6,output_w-26*hs,base);sprite(5,output_w-30*hs,base+11*hs);
    // as.a: with free attribute points the STAT plate blinks (5 of every 8 frames) above the HUD.
    if(model.progress.attribute_points>0 && (SDL_GetTicks()/50)%8<5)
        if(const auto stat=textures.find(0x51010);stat!=textures.end()) {
            int iw=0,ih=0;SDL_QueryTexture(stat->second,nullptr,nullptr,&iw,&ih);
            SDL_Rect dst{5*hs,base+9*hs-ih*hs,iw*hs,ih*hs};SDL_RenderCopy(renderer,stat->second,nullptr,&dst);
        }
    // The art carries phone keys (1 / 3 on the guardian slots, 9 and # on the item box);
    // repaint those spots with the platform's buttons.
    const auto relabel=[&](int fx,int fy,SDL_Rect cover,SDL_Color fill,SDL_Color ink,const std::string& key,int centre) {
        hud.rect({fx+cover.x*hs,fy+cover.y*hs,cover.w*hs,cover.h*hs},fill);
        if(playstation_shape(key)>=0)
            key_cap(hud,key,fx+(centre-3)*hs,fy+cover.y*hs,7*hs,ink,true);
        else
            draw_micro_text(renderer,key,fx+(centre-micro_text_width(key)/2)*hs,fy+(cover.y+(cover.h-5)/2)*hs,hs,ink);
    };
    const SDL_Color frame_light{191,191,127,255},frame_dark{95,95,63,255},frame_text{223,223,191,255},frame_navy{31,31,63,255};
    // Opaque dark plates keep the guardian buttons readable over the pale HUD art.
    const SDL_Color button_plate{16,12,24,255},button_ink{239,239,223,255};
    // Plates with one HUD pixel cut from each corner, so they do not read as hard squares when scaled.
    const auto plate=[&](SDL_Rect r){hud.rect({r.x+hs,r.y,r.w-2*hs,r.h},button_plate);hud.rect({r.x,r.y+hs,r.w,r.h-2*hs},button_plate);};
    plate({0,base+22*hs,7*hs,9*hs});
    plate({22*hs,base+22*hs,7*hs,9*hs});
    relabel(0,base+12*hs,{0,11,7,7},button_plate,button_ink,model.key_guardian_calm,3);
    relabel(22*hs,base+12*hs,{0,11,7,7},button_plate,button_ink,model.key_guardian_wild,3);
    relabel(output_w-26*hs,base,{4,4,3,5},frame_dark,frame_text,model.key_item_use,5);
    relabel(output_w-26*hs,base,{13,4,7,5},frame_navy,frame_light,model.key_item_next,16);
    if(model.guardians.equipped<model.guardians.slots.size()) {
        const auto& active=model.guardians.slots[model.guardians.equipped];
        // as.a: black 14x14 drawRect, the ce.c icon, a 0xBF1F3F drawRect while p.c[skill] runs and
        // ce.C (img/etcui 11) clipped to 13 x 13*p.c/p.b from (x+1, y+1).
        const auto skill_icon=[&](int skill,int x,int slot) {
            if(skill<0) return;
            const SDL_Rect box{x*hs,base+15*hs,15*hs,15*hs};
            hud.outline(box,{0,0,0,255},hs);
            sprite(500+6+active->id*4+skill,x*hs,base+15*hs);
            const int left=model.guardian_cooldown[std::size_t(slot)],full=model.guardian_cooldown_max[std::size_t(slot)];
            if(left==0 || full<=0) return;
            hud.outline(box,{191,31,63,255},hs);
            const int shown=13*left/full;
            if(const auto cover=textures.find(0x52020);cover!=textures.end() && shown>0) {
                SDL_Rect src{1,1,13,shown},dst{(x+1)*hs,base+16*hs,13*hs,shown*hs};
                SDL_RenderCopy(renderer,cover->second,&src,&dst);
            }
        };
        if(active) {skill_icon(active->calm,7,0);skill_icon(active->wild,29,1);}
    }
    const int bx=47*hs,bw=std::max(1,output_w-67*hs);
    const auto ratio=[&](int current,int maximum) {
        return maximum>0?int(std::int64_t(std::clamp(current,0,maximum))*bw/maximum):0;
    };
    hud.rect({bx,base+20*hs,bw,4*hs},{0,0,0,255});
    const int hpw=ratio(model.vitals.hp,model.vitals.max_hp);
    hud.rect({bx,base+20*hs,hpw,4*hs},{255,0,0,255});
    hud.rect({bx,base+21*hs,hpw,2*hs},{255,159,63,255});
    hud.rect({bx,base+27*hs,bw,2*hs},{0,0,0,255});
    hud.rect({bx,base+27*hs,ratio(model.vitals.sp,model.vitals.max_sp),2*hs},{63,255,191,255});
    // as: full-width EXP bar beneath HP/SP.
    hud.rect({0,base+31*hs,output_w,5*hs},{159,159,127,255});
    hud.rect({2*hs,base+32*hs,output_w-4*hs,3*hs},{63,63,63,255});
    const int exp_width=int(std::int64_t(std::clamp(model.progress.experience,0,model.progress.next_experience()))*
                            (output_w-6*hs)/model.progress.next_experience());
    hud.rect({3*hs,base+33*hs,exp_width,hs},{191,191,127,255});
    hud_digits(model.vitals.hp,bx+bw-3*hs,base+18*hs); // r.c(hp, 46+o-2, y+18, RIGHT)
    if(model.quick_item_type>=0) {
        const auto found=textures.find(200+model.quick_item_type);
        if(found!=textures.end()) {
            int iw=0,ih=0;SDL_QueryTexture(found->second,nullptr,nullptr,&iw,&ih);
            SDL_Rect dst{output_w-(10+iw/2)*hs,base+(19-ih/2)*hs,iw*hs,ih*hs};
            SDL_RenderCopy(renderer,found->second,nullptr,&dst);
        }
        hud_digits(model.quick_item_count,output_w-4*hs,base+22*hs);
    }
}

// as.a enemy panel (105x27 at scale es): name by rank colour, LV and digits, red HP strip.
void draw_enemy_panel(SDL_Renderer* renderer,const VisualUiModel& model,const UiTextureMap& textures,
    SDL_Texture* font_texture,const BitmapFont& font,int x,int y,int es) {
    Painter enemy{renderer,textures,font_texture,font,es};
    const SDL_Color name_color=model.enemy_rank==1?SDL_Color{255,127,47,255}:
        model.enemy_rank==2?SDL_Color{255,255,31,255}:SDL_Color{255,255,255,255};
    cb_frame(enemy,x/es,y/es,105,27);
    enemy.rect({x+2*es,y+2*es,101*es,23*es},{95,31,63,255});
    const auto width_of=[&](const std::string& text){int w=0;
        for(const auto& g:layout_bitmap_text(font,text,0,0,100000,es)) w=std::max(w,g.x+int(g.source_width)*es);
        return w;};
    // cb.a(...,0,1,...): name centred in the 101px box, one pixel below its top.
    const int name_w=std::min(width_of(model.enemy_name),101*es);
    enemy.line(model.enemy_name,{x+2*es+(101*es-name_w)/2,y+3*es,name_w+es,10*es},name_color);
    // r.a(bh.c, 1, 16): white "LV" on a black plate, then the level digits right after it.
    const std::string lv=bank_text(model,3949,"LV");
    const int lv_w=width_of(lv);
    enemy.rect({x+2*es,y+17*es,lv_w+2*es,(int(font.height)+2)*es},{0,0,0,255});
    enemy.text(lv,{x+3*es,y+18*es,lv_w+es,int(font.height)*es},{255,255,255,255});
    const std::string level=std::to_string(std::max(0,model.enemy_level));
    const int lx=x+5*es+lv_w;
    if(const auto strip=textures.find(0x50005);strip!=textures.end())
        for(std::size_t i=0;i<level.size();++i) {
            SDL_Rect src{(level[i]-'0')*5,0,5,7},dst{lx+int(i)*4*es,y+18*es,5*es,7*es};
            SDL_RenderCopy(renderer,strip->second,&src,&dst);
        }
    else enemy.text(level,{lx,y+18*es,30*es,8*es},{255,255,255,255});
    const int fill=model.enemy_hp>0 && model.enemy_max_hp>0?
        std::clamp(77*(model.enemy_hp-1)/model.enemy_max_hp-4,0,72):0;
    enemy.rect({x+29*es,y+22*es,fill*es,2*es},{255,63,47,255});
}

// Bottom screen (3DS, 320x240): dark plum with thin bronze frames (port design, 2026-11).
namespace bottom_ui {
constexpr SDL_Color plum{34,18,32,255},panel{52,26,44,255},shade{20,10,18,255},
    bronze{196,154,90,255},bronze_mid{140,100,56,255},bronze_dark{74,48,30,255},text_ink{236,224,200,255};
// 1px dark rim, bronze ring with a lit top-left and shaded bottom-right edge, panel fill.
void frame(const Painter& p,SDL_Rect r,SDL_Color fill=panel) {
    p.rect(r,bronze_dark);
    p.rect({r.x+1,r.y+1,r.w-2,r.h-2},bronze_mid);
    p.rect({r.x+1,r.y+1,r.w-2,1},bronze);p.rect({r.x+1,r.y+1,1,r.h-2},bronze);
    p.rect({r.x+2,r.y+2,r.w-4,r.h-4},bronze_dark);
    p.rect({r.x+3,r.y+3,r.w-6,r.h-6},fill);
}
int text_width(const BitmapFont& font,const std::string& text,int scale) {
    int w=0;
    for(const auto& g:layout_bitmap_text(font,text,0,0,100000,scale)) w=std::max(w,g.x+int(g.source_width)*scale);
    return w;
}
void bar(const Painter& p,SDL_Rect r,int value,int maximum,SDL_Color colour,SDL_Color light) {
    p.rect(r,{0,0,0,255});
    const int fill=maximum>0?int(std::int64_t(std::clamp(value,0,maximum))*r.w/maximum):0;
    if(fill<=0) return;
    p.rect({r.x,r.y,fill,r.h},colour);
    p.rect({r.x,r.y+1,fill,1},light);
}
} // namespace bottom_ui

static void draw_bottom_backdrop(SDL_Renderer* renderer,const UiTextureMap& textures,SDL_Texture* font_texture,const BitmapFont& font) {
    using namespace bottom_ui;
    Painter p{renderer,textures,font_texture,font,1};
    p.rect({0,0,320,240},plum);
    p.rect({0,0,320,1},bronze_dark);p.rect({0,239,320,1},bronze_dark);
    p.rect({0,0,1,240},bronze_dark);p.rect({319,0,1,240},bronze_dark);
    p.rect({1,1,318,1},bronze_mid);p.rect({1,238,318,1},bronze_mid);
    p.rect({1,1,1,238},bronze_mid);p.rect({318,1,1,238},bronze_mid);
}

static std::vector<UiHit> g_ui_hits;
static std::vector<UiHit> g_bottom_hits; // the bottom-screen field layout (kept while its picture is reused)
const std::vector<UiHit>& bottom_screen_hits() { return g_bottom_hits; }
static bool g_ui_hits_blocked=false; // a question or notice is up: only its A / B answers it
const std::vector<UiHit>& visual_ui_hits() {
    // While a question, notice or small choice list is up, only it and the key buttons answer.
    static std::vector<UiHit> shown;
    const bool choice=std::any_of(g_ui_hits.begin(),g_ui_hits.end(),[](const UiHit& h){return h.kind==6;});
    shown.clear();
    for(const auto& h:g_ui_hits) if(!(g_ui_hits_blocked || choice) || h.kind==6 || h.kind==7 || h.kind==10) shown.push_back(h);
    return shown;
}

// Bottom-screen minimap geometry, shared with main.cpp (3DS redraws just the hero's cell on a step).
static void bottom_minimap_layout(const MinimapView& map,int* x0,int* y0,int* cell) {
    constexpr int area_top=32,area_h=116; // between the enemy bar (y 30) and the HP/SP row (y 152)
    *cell=std::max(1,std::min(280/map.width,(area_h-8)/map.height));
    *x0=160-map.width**cell/2;*y0=area_top+(area_h-map.height**cell)/2;
}
bool bottom_minimap_cell(const MinimapView& map,int x,int y,SDL_Rect* out) {
    if(map.width<=0 || map.height<=0 || x<0 || y<0 || x>=map.width || y>=map.height) return false;
    int x0=0,y0=0,cell=1;bottom_minimap_layout(map,&x0,&y0,&cell);
    *out={x0+x*cell,y0+y*cell,cell,cell};return true;
}

// Touch layout shared with main.cpp (3DS input): the five buttons along the bottom.
SDL_Rect bottom_button_rect(int index) { return {7+index*62,182,58,52}; }

// Bottom screen (3DS): the game menu, shop and services; otherwise the enemy bar on top, the framed
// minimap (indoors: a plate with the location name), the HP/SP row and five touch buttons:
// calm guardian, wild guardian, menu, quick item, next item.
void draw_bottom_screen(SDL_Renderer* renderer,const VisualUiModel& model,const UiTextureMap& textures,
    SDL_Texture* font_texture,const BitmapFont& font,bool hud,bool menu,const std::function<void(SDL_Rect)>& hero_preview) {
    using namespace bottom_ui;
    SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_BLEND);
    if(menu) {draw_visual_ui(renderer,model,textures,font_texture,font,320,240,hero_preview);return;}
    g_bottom_hits.clear();
    const unsigned layers=model.bottom_layers;
    if(layers&1u) draw_bottom_backdrop(renderer,textures,font_texture,font);
    if(!hud) {
        // The logo page waits on the top screen: say a tap starts too (as on the DS).
        if((layers&1u) && model.screen==VisualScreen::title_menu && model.startup.page==TitlePage::logo) {
            const Painter p{renderer,textures,font_texture,font,1};
            const std::string prompt=tr_text(model,"Touch or press any key","Коснитесь экрана или нажмите кнопку");
            const int w=text_width(font,prompt,1);
            p.text(prompt,{160-w/2,120-int(font.height)/2,w+1,int(font.height)+1},gold);
        }
        return;
    }
    Painter p{renderer,textures,font_texture,font,1};
    const int fh=int(font.height);
    const auto right_text=[&](const std::string& text,int right,int y,SDL_Color c){
        const int w=text_width(font,text,1);p.text(text,{right-w,y,w+1,fh+1},c);};
    // Middle: minimap or the indoor plate, between the enemy bar (y 30) and the HP/SP row (y 152).
    constexpr int area_top=32,area_h=116;
    const bool talking=model.screen==VisualScreen::dialogue || model.screen==VisualScreen::choice ||
        model.screen==VisualScreen::notice;
    if(!(layers&2u)) {}
    else if(talking) {
        // The dialogue window (the top screen has none): face at the speaker's side (m/face, by the
        // sign of the id), the page in gold; questions, item messages and notices centred in white.
        const auto layout=calculate_bottom_dialogue_layout(fh,model.portrait);
        const SDL_Rect box{layout.panel.x,layout.panel.y,layout.panel.w,layout.panel.h};
        frame(p,box,{0,0,0,255});
        if(model.portrait)
            if(const auto face=textures.find(100+std::abs(model.portrait)-1);face!=textures.end()) {
                const SDL_Rect dst{model.portrait>0?box.x+5:box.x+box.w-55,box.y+box.h-64,50,60};
                SDL_RenderCopy(renderer,face->second,nullptr,&dst);
            }
        const SDL_Rect inner{layout.inner.x,layout.inner.y,layout.inner.w,layout.inner.h};
        if(model.screen==VisualScreen::dialogue && !model.item_box) {
            p.text(model.text,inner,gold,model.scroll_lines,model.dialogue_visible_end);
            int lines=1;
            for(const auto& g:layout_bitmap_text(font,model.text,0,0,inner.w,1)) lines=std::max(lines,g.y/fh+1);
            if(lines>model.scroll_lines+layout.visible_lines) p.rect({box.x+box.w-12,box.y+box.h-10,5,3},gold);
        } else {
            int lines=1;
            for(const auto& g:layout_bitmap_text(font,model.text,0,0,inner.w,1)) lines=std::max(lines,g.y/fh+1);
            const int h=std::min(lines*fh,box.h-10);
            p.text(model.text,{inner.x,box.y+(box.h-h)/2,inner.w,h+1},text_ink);
        }
    }
    else if(model.bottom_minimap &&model.bottom_minimap->width>0 && model.bottom_minimap->height>0) {
        const auto& map=*model.bottom_minimap;
        int x0=0,y0=0,cell=1;bottom_minimap_layout(map,&x0,&y0,&cell);
        const int w=map.width*cell,h=map.height*cell;
        frame(p,{x0-4,y0-4,w+8,h+8},{0,0,0,255});
        // One map-sized texture stretched with nearest filtering (a fill per cell cost ~15 ms).
        static SDL_Texture* grid=nullptr;
        static int grid_w=0,grid_h=0;
        static std::vector<Uint32> pixels;
        if(!grid || grid_w!=map.width || grid_h!=map.height) {
            if(grid) SDL_DestroyTexture(grid);
            grid=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STATIC,map.width,map.height);
            grid_w=map.width;grid_h=map.height;
            if(grid) {SDL_SetTextureBlendMode(grid,SDL_BLENDMODE_NONE);SDL_SetTextureScaleMode(grid,SDL_ScaleModeNearest);}
        }
        if(grid) {
            pixels.assign(std::size_t(map.width)*std::size_t(map.height),0xFF000000u);
            for(std::size_t i=0;i<pixels.size() && i<map.cells.size();++i) pixels[i]=0xFF000000u|map.cells[i];
            if(map.hero_x>=0 && map.hero_y>=0) { // -1: drawn later by the caller
                const std::size_t hero=std::size_t(map.hero_y)*std::size_t(map.width)+std::size_t(map.hero_x);
                if(hero<pixels.size()) pixels[hero]=0xFFFF3F3Fu;
            }
            SDL_UpdateTexture(grid,nullptr,pixels.data(),map.width*4);
            const SDL_Rect dst{x0,y0,w,h};
            SDL_RenderCopy(renderer,grid,nullptr,&dst);
        }
    } else if(!model.location_name.empty()) {
        const int tw=text_width(font,model.location_name,1);
        const int bw=std::clamp(tw+28,120,300),bh=fh+34,bx=160-bw/2,by=area_top+(area_h-bh)/2;
        frame(p,{bx,by,bw,bh});
        p.text(model.location_name,{160-tw/2,by+7,tw+1,fh+1},text_ink);
        // A small stone arch with a wooden door under the name.
        const int dx=160-8,dy=by+fh+11;
        const SDL_Color stone{150,140,110,255},stone_dark{90,84,64,255},wood{96,56,30,255},wood_dark{58,32,18,255};
        p.rect({dx+3,dy,10,2},stone);p.rect({dx+1,dy+2,14,2},stone);p.rect({dx,dy+4,16,12},stone);
        p.rect({dx+3,dy+3,10,13},stone_dark);p.rect({dx+4,dy+4,8,12},wood);
        p.rect({dx+6,dy+4,1,12},wood_dark);p.rect({dx+9,dy+4,1,12},wood_dark);p.rect({dx-2,dy+15,20,2},stone_dark);
    }
    if(layers&4u) {
        // HP / SP row with numbers, and the EXP line along its lower edge.
        frame(p,{4,152,312,26});
        const auto& v=model.vitals;
        p.text("HP",{11,158,20,fh+1},text_ink);
        bar(p,{30,160,60,5},v.hp,v.max_hp,{232,52,40,255},{255,140,90,255});
        right_text(std::to_string(std::max(0,v.hp))+"/"+std::to_string(std::max(0,v.max_hp)),148,158,text_ink);
        p.rect({159,157,1,14},bronze_mid);
        p.text("SP",{168,158,20,fh+1},text_ink);
        bar(p,{187,160,60,5},v.sp,v.max_sp,{40,200,160,255},{120,255,210,255});
        right_text(std::to_string(std::max(0,v.sp))+"/"+std::to_string(std::max(0,v.max_sp)),308,158,text_ink);
        const int next=std::max(1,model.progress.next_experience());
        p.rect({8,173,304,1},shade);
        p.rect({8,173,int(std::int64_t(std::clamp(model.progress.experience,0,next))*304/next),1},{191,191,127,255});
        // Five buttons: icons drawn 2x, the platform key in the corner.
        const auto icon=[&](int key,SDL_Rect button,int scale) {
            const auto found=textures.find(key);if(found==textures.end()) return false;
            int iw=0,ih=0;SDL_QueryTexture(found->second,nullptr,nullptr,&iw,&ih);
            const SDL_Rect dst{button.x+(button.w-iw*scale)/2,button.y+(button.h-ih*scale)/2,iw*scale,ih*scale};
            SDL_RenderCopy(renderer,found->second,nullptr,&dst);return true;
        };
        const Painter big{renderer,textures,font_texture,font,2};
        const auto key_label=[&](const std::string& key,SDL_Rect button) {
            if(key.empty()) return;
            const int w=text_width(font,key,2);
            big.text(key,{button.x+button.w-w-5,button.y+button.h-2*fh-3,w+2,2*fh+2},text_ink);
        };
        const Guardian* active=model.guardians.equipped<model.guardians.slots.size() && model.guardians.slots[model.guardians.equipped]?
            &*model.guardians.slots[model.guardians.equipped]:nullptr;
        for(int slot=0;slot<2;++slot) {
            const SDL_Rect b=bottom_button_rect(slot);
            frame(p,b);
            const int skill=active?(slot==0?active->calm:active->wild):-1;
            if(skill>=0) {
                const SDL_Rect art{b.x-6,b.y,b.w,b.h};
                icon(500+6+active->id*4+skill,art,2);
                const int left=model.guardian_cooldown[std::size_t(slot)],full=model.guardian_cooldown_max[std::size_t(slot)];
                if(left>0 && full>0) { // cooldown: the icon darkens from the top, like ce.C over the HUD icon
                    const int shown=30*left/full;
                    p.rect({art.x+(art.w-30)/2,art.y+(art.h-30)/2,30,shown},{0,0,0,150});
                }
            }
            key_label(slot==0?model.key_guardian_calm:model.key_guardian_wild,b);
        }
        { // menu: three bars and the word; while attribute points wait the original STAT plate
          // (img/etcui 4, as.a over the HUD) takes the bars' place, steady as on the DS (a blink
          // redrew the button row every few frames)
            const SDL_Rect b=bottom_button_rect(2);
            frame(p,b);
            const auto stat=model.progress.attribute_points>0?textures.find(0x51010):textures.end();
            if(stat!=textures.end()) {
                int iw=0,ih=0;SDL_QueryTexture(stat->second,nullptr,nullptr,&iw,&ih);
                const int z=iw*2<=b.w-8 && ih*2<=20?2:1;
                const SDL_Rect dst{b.x+(b.w-iw*z)/2,b.y+18-ih*z/2,iw*z,ih*z};
                SDL_RenderCopy(renderer,stat->second,nullptr,&dst);
            } else for(int k=0;k<3;++k) p.rect({b.x+19,b.y+11+k*5,20,2},text_ink);
            const std::string word=tr_text(model,"MENU","МЕНЮ");
            const int w=text_width(font,word,1);
            p.text(word,{b.x+(b.w-w)/2,b.y+30,w+1,fh+1},text_ink);
        }
        { // quick item with its count in a small framed box
            const SDL_Rect b=bottom_button_rect(3);
            frame(p,b);
            if(model.quick_item_type>=0) {
                icon(200+model.quick_item_type,{b.x-6,b.y-4,b.w,b.h},2);
                const std::string count=std::to_string(std::max(0,model.quick_item_count));
                const int w=text_width(font,count,1);
                const SDL_Rect box{b.x+b.w-w-12,b.y+b.h-fh-11,w+8,fh+6};
                frame(p,box,shade);
                p.text(count,{box.x+4,box.y+3,w+1,fh+1},text_ink);
            }
        }
        { // next item
            const SDL_Rect b=bottom_button_rect(4);
            frame(p,b);
            const int cx=b.x+b.w/2-4,cy=b.y+b.h/2;
            for(int k=0;k<9;++k) p.rect({cx+k,cy-8+k,1,17-2*k},text_ink);
        }
    }
    // While the top screen shows dialogue, a Yes/No question or a message, the button row answers it.
    const bool choice=model.screen==VisualScreen::choice;
    const bool message=model.screen==VisualScreen::notice || (model.screen==VisualScreen::dialogue && model.item_box);
    if((layers&4u) && (choice || message || model.screen==VisualScreen::dialogue)) {
        const Painter latin{renderer,textures,model.latin_font_texture?model.latin_font_texture:font_texture,
                            model.latin_font?*model.latin_font:font,1};
        std::vector<std::pair<std::string,std::string>> keys; // key cap ("" for none), label
        if(choice) keys={{model.choice_yes?"A":"",bank_text(model,3915,"Yes")},{model.choice_yes?"":"A",bank_text(model,3916,"No")}};
        else if(message) keys={{"A",bank_text(model,3908,"Ok")}};
        else keys={{"A",bank_text(model,3911,"Next")},{"B",bank_text(model,3910,"Skip")}};
        // Menu-sized buttons (24px) in the middle of the button row; the whole row half is the tap target.
        const int n=int(keys.size()),gap=4,w=(308-(n-1)*gap)/n;
        p.rect({4,180,312,58},plum);
        for(int i=0;i<n;++i) {
            const SDL_Rect b{6+i*(w+gap),196,w,24};
            frame(p,b,choice && (i==0)==model.choice_yes?SDL_Color{95,31,63,255}:bottom_ui::panel);
            const auto& key=keys[std::size_t(i)];
            const int cap=key.first.empty()?0:key_cap(latin,key.first,0,0,10,text_ink,false)+4;
            const int total=cap+text_width(font,key.second,1);
            const int x=b.x+std::max(6,(b.w-total)/2);
            if(cap) key_cap(latin,key.first,x,b.y+7,10,text_ink,true);
            p.text(key.second,{x+cap,b.y+8,b.x+b.w-(x+cap)-4,fh+1},text_ink);
            const SDL_Rect target{b.x,182,b.w,54};
            if(choice) g_bottom_hits.push_back({target,11,i,false,false});
            else g_bottom_hits.push_back({target,7,key.first=="B"?1:0,false,false});
        }
    }
    if((layers&8u) && !model.enemy_name.empty()) { // enemy bar along the top
        frame(p,{4,4,312,24});
        const std::string head=model.enemy_name+"  "+bank_text(model,3949,"LV")+" "+std::to_string(std::max(0,model.enemy_level));
        const SDL_Color name_color=model.enemy_rank==1?SDL_Color{255,127,47,255}:
            model.enemy_rank==2?SDL_Color{255,255,31,255}:text_ink;
        p.text(head,{11,10,140,fh+1},name_color);
        bar(p,{152,13,100,6},model.enemy_hp,model.enemy_max_hp,{232,52,40,255},{255,140,90,255});
        right_text(std::to_string(std::max(0,model.enemy_hp))+"/"+std::to_string(std::max(0,model.enemy_max_hp)),308,10,text_ink);
    }
}

void draw_visual_ui(SDL_Renderer* renderer,const VisualUiModel& model,
    const UiTextureMap& textures,SDL_Texture* font_texture,const BitmapFont& font,
    int output_w,int output_h,const std::function<void(SDL_Rect)>& hero_preview) {
    const int s=std::max(1,std::min(output_w/640,output_h/360));
    Painter p{renderer,textures,font_texture,font,s};
    g_buttons=model.buttons;
    // 3DS: the top screen's field pass must not wipe the touch targets of the bottom-screen menu,
    // which is redrawn only on input.
    if(!model.hud_elsewhere || model.bottom_pass) {
        g_ui_hits.clear();
        g_ui_hits_blocked=model.confirming || !model.notice.empty();
    }
    SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_BLEND);
    const bool menu_screen=model.screen==VisualScreen::game_menu || model.screen==VisualScreen::shop ||
        (model.screen==VisualScreen::service && model.service);
    if(menu_screen && !model.world_hidden && !model.bottom_pass) capture_menu_backdrop(renderer,output_w,output_h);
    else if(!menu_screen) release_menu_backdrop();
    if(model.screen==VisualScreen::ending && model.ending) {
        // as.d: story pages on black (after a white fade); as.e: staff roll beside img/end.
        const auto& v=*model.ending;
        const int ms=std::max(1,std::min(output_w/240,output_h/220));
        const int lw=output_w/ms,lh=output_h/ms;
        Painter m{renderer,textures,font_texture,font,ms};
        const auto width_of=[&](const std::string& text){int w=0;
            for(const auto& g:layout_bitmap_text(font,text,0,0,10000,1)) w=std::max(w,g.x+int(g.source_width));
            return w;};
        const auto line=[&](const std::string& text,int y) {
            m.text(text,{(lw/2-width_of(text)/2)*ms,y*ms,(width_of(text)+2)*ms,(int(font.height)+1)*ms},{255,255,255,255});
        };
        // anchor: 17 top-centre, 40 bottom-right, 6 left-vcentre
        const auto picture=[&](int key,int x,int y,int anchor,bool mirror) {
            const auto found=textures.find(key);if(found==textures.end()) return;
            int iw=0,ih=0;SDL_QueryTexture(found->second,nullptr,nullptr,&iw,&ih);
            if(anchor==17) x-=iw/2; else if(anchor==40) {x-=iw;y-=ih;} else if(anchor==6) y-=ih/2;
            SDL_Rect dst{x*ms,y*ms,iw*ms,ih*ms};
            SDL_RenderCopyEx(renderer,found->second,nullptr,&dst,0.0,nullptr,mirror?SDL_FLIP_HORIZONTAL:SDL_FLIP_NONE);
        };
        if(v.fade>=0) {const auto c=Uint8(v.fade);m.rect({0,0,output_w,output_h},{c,c,c,255});return;}
        m.rect({0,0,output_w,output_h},{0,0,0,255});
        if(v.credits) {
            picture(0x52000,0,lh/2,6,false);
            for(const auto& roll:v.rolling) line(roll.first,roll.second);
            return;
        }
        int y=lh/2-(int(v.page.size())+1)*15/2;
        if(v.hero==6 && (v.first==2 || v.first==6 || v.first==9 || v.first==13)) {
            // Ronin: grey band with his face (m/face 0, mirrored by br.c); page 9 slides in face 8.
            m.rect({0,15*ms,output_w,40*ms},{191,191,191,255});
            SDL_Rect clip{0,15*ms,output_w,40*ms};SDL_RenderSetClipRect(renderer,&clip);
            picture(100,lw/4,5,17,true);
            if(v.first==9) picture(108,lw/4*3+(v.counter>=27?(v.counter-27)*10:0),5,17,true);
            SDL_RenderSetClipRect(renderer,nullptr);
            y+=30;
        } else if(v.hero==8) picture(117,lw,lh,40,false);
        for(const auto& text:v.page) {line(text,y);y+=15;}
        return;
    }
    if(model.screen==VisualScreen::title_menu) {
        const auto& menu=model.startup;
        const auto page=menu.page;
        const int ms=std::max(1,std::min(output_w/240,output_h/220));
        Painter m{renderer,textures,font_texture,font,ms};
        // Strings come from the JAR: language bank (bh) and sgui/com.tdf (ce.g), so localized JARs translate them.
        const auto bank=[&](int i,const char* fallback){return language_text(model,i,fallback);};
        const auto com=[&](int i,const char* fallback){
            return std::size_t(i)<model.common_text.size() && !model.common_text[std::size_t(i)].empty()?
                model.common_text[std::size_t(i)]:std::string(fallback);};
        const auto trim=[](std::string t){while(!t.empty() && t.back()==' ') t.pop_back();return t;};
        // Port-only strings: English, or Russian re-encoded for the Russian font.
        const auto tr=[&](const char* en,const char* ru){return model.localized?std::string(ru):std::string(en);};
        Painter latin{renderer,textures,model.latin_font_texture?model.latin_font_texture:font_texture,
                      model.latin_font?*model.latin_font:font,ms};
        // Button prefix in the Latin font, then the game's own word.
        const auto key_label=[&](const char* key,const std::string& word,SDL_Rect r,SDL_Color c) {
            const int kw=std::max(key_cap(latin,key,r.x,r.y,r.h,c,true)+3*ms,14*ms);
            m.text(word,{r.x+kw,r.y,std::max(0,r.w-kw),r.h},c);
        };
        const auto sprite=[&](int key,int x,int y,int scale=0) {
            const auto found=textures.find(key);if(found==textures.end()) return;
            int iw=0,ih=0;SDL_QueryTexture(found->second,nullptr,nullptr,&iw,&ih);
            const int z=scale?scale:ms;SDL_Rect dst{x,y,iw*z,ih*z};
            SDL_RenderCopy(renderer,found->second,nullptr,&dst);
        };
        // af type 1/2 box centred on the screen (80% of 240 px wide).
        const auto af_box=[&](const std::string& text) {
            const int lines=1+int(std::count(text.begin(),text.end(),'\n'));
            const int w=192*ms,h=(lines*int(font.height)+10)*ms,x=output_w/2-w/2,y=output_h/2-h/2;
            m.rect({x,y,w,h},{31,31,63,255});m.rect({x+ms,y+ms,w-2*ms,h-2*ms},{95,63,63,255});
            m.rect({x+2*ms,y+2*ms,w-4*ms,h-4*ms},{63,31,63,255});
            m.text(text,{x+5*ms,y+5*ms,w-10*ms,h-8*ms},{255,255,255,255});
        };
        if(page==TitlePage::quit_confirm) {
            m.rect({0,0,output_w,output_h},{0,0,0,255});
            af_box(bank(3904,"Are You Sure?"));
            key_label("A",bank(3915,"Yes"),{2*ms,output_h-10*ms,80*ms,10*ms},gold);
            key_label("B",bank(3916,"No"),{output_w-58*ms,output_h-10*ms,58*ms,10*ms},gold);
            return;
        }
        if(page==TitlePage::splash) {
            m.rect({0,0,output_w,output_h},{255,255,255,255});
            if(const auto found=textures.find(0x52010);found!=textures.end()) {
                int iw=0,ih=0;SDL_QueryTexture(found->second,nullptr,nullptr,&iw,&ih);
                const int y=(menu.splash.y-menu.splash.speed)*ms; // anchor 3: centred on (r.i, a - a`S)
                SDL_Rect dst{output_w/2-iw*ms/2,y-ih*ms/2,iw*ms,ih*ms};SDL_RenderCopy(renderer,found->second,nullptr,&dst);
            }
            return;
        }
        if(page==TitlePage::logo) {
            m.rect({0,0,output_w,output_h},{255,255,255,255});
            const int lx=output_w/2-60*ms,ly=output_h/2-68*ms;
            sprite(602,lx,ly+25*ms);sprite(603,lx+52*ms,ly+25*ms);sprite(604,lx+93*ms,ly+2*ms);
            // Feathers: k[frame] (left) and mirrored k[frame+5] (right), anchor HCENTER|BOTTOM.
            for(int i=0;i<2;++i) {
                const auto& f=menu.logo.feathers[std::size_t(i)];
                if(!f.initialised) continue;
                const auto found=textures.find(650+TitleLogoAnimation::frame(f.phase));
                if(found==textures.end()) continue;
                int iw=0,ih=0;SDL_QueryTexture(found->second,nullptr,nullptr,&iw,&ih);
                SDL_Rect dst{(f.x-iw/2)*ms,(f.y-ih)*ms,iw*ms,ih*ms};
                SDL_RenderCopyEx(renderer,found->second,nullptr,&dst,0,nullptr,i?SDL_FLIP_HORIZONTAL:SDL_FLIP_NONE);
            }
            // bh.q (3950) in black at (r.i, r.h-45), visible two steps out of four.
            if(model.logo_prompt && menu.logo.prompt_visible()) {
                const std::string prompt=bank(3950,"PRESS ANY KEY");
                int width=0;
                for(const auto& glyph:layout_bitmap_text(font,prompt,0,0,300*ms,ms))
                    width=std::max(width,glyph.x+int(glyph.source_width)*ms);
                m.text(prompt,{output_w/2-width/2,output_h-45*ms,width+ms,12*ms},SDL_Color{0,0,0,255});
            }
            return;
        }
        if(page==TitlePage::loading || page==TitlePage::guardian_intro) {
            m.rect({0,0,output_w,output_h},{0,0,0,255});
            if(page==TitlePage::guardian_intro) {
                SDL_Rect box{output_w/2-90*ms,output_h/2-20*ms,180*ms,40*ms};
                m.rect(box,ink);m.outline(box,{95,95,127,255},ms);
                m.text(com(16,"GUARDIANS")+"\n"+com(13,"SELECTION."),{box.x+4*ms,box.y+4*ms,box.w-8*ms,box.h-8*ms},gold);
                key_label("A",bank(3911,"Next"),{2*ms,output_h-10*ms,100*ms,10*ms},gold);
            } else {
                m.text(com(37,"NOW LOADING")+" ...",{output_w/2-65*ms,output_h/2-15*ms,140*ms,12*ms},gold);
                SDL_Rect bar{output_w/2-65*ms,output_h/2,130*ms,5*ms};m.outline(bar,muted,ms);
                m.rect({bar.x+ms,bar.y+ms,(bar.w-2*ms)*std::clamp(model.loading_progress,0,100)/100,bar.h-2*ms},muted);
                m.text(com(38,"Hands-On Mobile"),{output_w/2-65*ms,output_h/2+55*ms,150*ms,12*ms},gold);
            }
            return;
        }
        m.rect({0,0,output_w,output_h},ink);
        const int mx=output_w/2-76*ms,my=output_h/2-84*ms;
        // bf.c / bf.b: 12px corners, four 32px spans, 24px body rows.
        const auto paper=[&](SDL_Rect r) {
            const auto row=[&](int first,int y) {
                sprite(800+first,r.x,y);
                for(int col=0;col<4;++col) sprite(801+first,r.x+(12+32*col)*ms,y);
                sprite(802+first,r.x+140*ms,y);
            };
            if(r.h==24*ms) {row(0,r.y);row(11,r.y+12*ms);return;}
            row(3,r.y);row(6,r.y+12*ms);
            const int rows=(r.h/ms-48)/24;
            // Original flat interior colour: 0xffcb79.
            m.rect({r.x+12*ms,r.y+36*ms,128*ms,24*rows*ms},{255,203,121,255});
            for(int i=0;i<rows;++i) {
                sprite(809,r.x,r.y+(36+24*i)*ms);
                sprite(810,r.x+140*ms,r.y+(36+24*i)*ms);
            }
            row(11,r.y+(36+24*rows)*ms);
        };
        const auto label=[&](std::string text,int y,SDL_Color color=SDL_Color{0,0,0,255}) {
            // The English heading font has capitals only. The Russian bank holds its headings in the
            // Russian big.mf's own codes: decoded to Cyrillic on load, encoded back here.
            if(model.localized) text=encode_russian_font_text(text);
            else for(auto& c:text) if(c>='a' && c<='z') c=char(c-'a'+'A');
            // bh.a(int): black text uses bh.d (white outline), white text bh.e (black outline).
            const bool white_text=color.r>127;
            const auto heading_texture=textures.find(white_text?850:851);
            const bool has_heading=model.startup_heading_font && heading_texture!=textures.end();
            const auto& heading_font=has_heading?*model.startup_heading_font:font;
            Painter heading{renderer,textures,has_heading?heading_texture->second:font_texture,heading_font,ms};
            if(has_heading) color=SDL_Color{255,255,255,255}; // the atlas carries both colours
            int width=0;
            for(const auto& glyph:layout_bitmap_text(heading_font,text,0,0,300*ms,ms))
                width=std::max(width,glyph.x+int(glyph.source_width)*ms);
            const int x=mx+(152*ms-width)/2;
            heading.text(text,{x,y,width+ms,int(heading_font.height)*ms},color);
        };
        // bh.a(g, heading, ...): headings are bh.a[][] = language 3920.. (New Game, Load Game, Options, Help, About).
        const auto help_topic=[&](int i){
            static const char* fallback[]={"Basic Controls","Guardian","Status","Item"};
            return std::size_t(i)<model.help_text.size() && !model.help_text[std::size_t(i)].empty()?
                model.help_text[std::size_t(i)]:std::string(fallback[std::clamp(i,0,3)]);};
        const std::string title=page==TitlePage::options?bank(3922,"Options"):page==TitlePage::help?bank(3923,"Help"):
            page==TitlePage::reading?(menu.help_topic==4?bank(3924,"About"):help_topic(menu.help_topic)):
            page==TitlePage::load_game?bank(3921,"Load Game"):bank(3920,"New Game");
        const bool main=page==TitlePage::menu;
        // Original screens: bf.c draws the 24px header at (p2,p3) and bf.b the body directly below
        // at p3+24 (main menu: body only). X/Y map original pixel offsets from (p2,p3).
        const auto X=[&](int o){return mx+o*ms;};
        const auto Y=[&](int o){return my+o*ms;};
        if(!main) {
            paper({mx,my,152*ms,24*ms});
            label(title,my+9*ms);
        }
        const int py=main?my:my+24*ms;
        // a.a: the Load Game list uses a three-row body (bf.b(..., 3)).
        paper({mx,py,152*ms,(main?168:page==TitlePage::load_game?120:page==TitlePage::options?124:144)*ms});
        // Top-left sprite, and a sprite centred on a point (anchor 3, HCENTER|VCENTER).
        const auto centred=[&](int key,int cx,int cy) {
            const auto found=textures.find(key);if(found==textures.end()) return;
            int iw=0,ih=0;SDL_QueryTexture(found->second,nullptr,nullptr,&iw,&ih);
            sprite(key,cx-iw*ms/2,cy-ih*ms/2);
        };
        const auto bottom_right=[&](int key,int rx,int by) { // anchor 40
            const auto found=textures.find(key);if(found==textures.end()) return;
            int iw=0,ih=0;SDL_QueryTexture(found->second,nullptr,nullptr,&iw,&ih);
            sprite(key,rx-iw*ms,by-ih*ms);
        };
        // 3DS bottom screen: touch targets, kind 12 a row / slot (index as the page's selection),
        // 13 YES (0) / NO (1).
        const auto touch=[&](SDL_Rect r,int kind,int index,bool selected) {
            if(model.bottom_pass) g_ui_hits.push_back({r,kind,index,selected,false});
        };
        // YES/NO under the etc[17] brush (by/bk): highlighted word is white.
        const auto yes_no=[&](int ox,int oy,int yes_x,int no_x,int brush_x) {
            touch({X(ox+yes_x)-6*ms,Y(oy+115),30*ms,17*ms},13,0,menu.yes);
            touch({X(ox+no_x)-6*ms,Y(oy+115),30*ms,17*ms},13,1,!menu.yes);
            sprite(817,X(ox+brush_x+(menu.yes?0:28)),Y(oy+118));
            m.text(com(14,"YES"),{X(ox+yes_x),Y(oy+121),26*ms,10*ms},menu.yes?white:gold);
            m.text(com(15,"NO"),{X(ox+no_x),Y(oy+121),26*ms,10*ms},menu.yes?gold:white);
        };
        if(main) {
            // bf.a: items every 16px from body+26, brush (etc 14/16/18) behind the selection.
            const std::string actions[]={bank(3920,"New Game"),bank(3921,"Load Game"),bank(3922,"Options"),
                                         bank(3923,"Help"),bank(3924,"About"),bank(3926,"Exit")};
            for(int i=0;i<6;++i) {
                const int ry=py+(26+i*16)*ms;
                touch({mx,ry-3*ms,152*ms,16*ms},12,i,menu.selection==i);
                const int brush=menu.brush_stage();
                if(menu.selection==i) {
                    const int key=814+2*brush;
                    const auto found=textures.find(key);
                    int iw=0,ih=0;if(found!=textures.end()) SDL_QueryTexture(found->second,nullptr,nullptr,&iw,&ih);
                    // (p2+155-w)>>1 with p2=44 on the 240px screen: (111-w)/2 from the panel edge.
                    sprite(key,mx+(111*ms-iw*ms)/2,py+(24+i*16)*ms);
                }
                // bh.a(g,i*2,...): the selected word turns white once the brush has finished.
                label(actions[i],ry,menu.selection==i && brush>=2?SDL_Color{255,255,255,255}:SDL_Color{0,0,0,255});
                if(i==1 && !menu.continue_available)
                    m.text("--",{mx+135*ms,ry,14*ms,10*ms},ink);
            }
        } else if(page==TitlePage::help) {
            // bt: topics in the large font (bh.a(true)), centred, 15px apart from p3+51; selection white.
            for(int i=0;i<4;++i) touch({mx,Y(51+i*15)-3*ms,152*ms,15*ms},12,i,menu.selection==i);
            for(int i=0;i<4;++i)
                label(help_topic(i),Y(51+i*15),menu.selection==i?SDL_Color{255,255,255,255}:SDL_Color{0,0,0,255});
        } else if(page==TitlePage::options) {
            // be: labels at p2+27, values 70px further, rows 20px apart from p3+56, etc[19] rules.
            // be: labels com 18..21, values com 60+speed / 3942 Fast, 3943 Normal / 3944 On, 3945 Off;
            // glb2/glb3 arrows at +42/+92 on every row.
            static const char* speed_fallback[]={"Slow","Normal","Fast"};
            // Port: a tab strip (selection 0) above the original 20px rows; each tab holds 2-3 settings.
            const std::string names[]={com(18,"Sound"),com(19,"Game"),com(20,"Speech"),com(21,"Camera"),tr("Language","Язык"),
                                       tr("Music","Музыка"),std::string(HEROES_LORE_LOW_POWER?"30 FPS":"60 FPS"),tr("Menus","Меню"),tr("Autosave","Автосейв"),tr("Rumble","Вибрация")};
            const std::string tabs[]={tr("Sound","Звук"),tr("Display","Экран"),tr("Interface","Интерфейс"),tr("Game","Игра")};
            const auto width_of=[&](const std::string& t){
                int w=0;for(const auto& glyph:layout_bitmap_text(font,t,0,0,300*ms,ms)) w=std::max(w,glyph.x+int(glyph.source_width)*ms);
                return w;};
            touch({mx,Y(37),152*ms,20*ms},12,0,menu.selection==0);
            for(int row=0;row<TitleMenu::option_rows(menu.options_tab);++row)
                touch({mx,Y(62+20*row),152*ms,20*ms},12,row+1,menu.selection==row+1);
            {
                const std::string& t=tabs[menu.options_tab];
                const int w=width_of(t);
                m.text(t,{X(76)-w/2,Y(41),w+ms,10*ms},menu.selection==0?white:gold);
                sprite(0x50002,X(28),Y(41));sprite(0x50003,X(123),Y(41));
                for(int i=0;i<TitleMenu::option_tab_count;++i) // page dots under the tab name
                    m.rect({X(76-(TitleMenu::option_tab_count*6)/2+i*6+1),Y(53),3*ms,3*ms},
                           i==menu.options_tab?SDL_Color{127,63,63,255}:SDL_Color{191,143,95,255});
                sprite(819,X(28),Y(58));
            }
            for(int row=0;row<TitleMenu::option_rows(menu.options_tab);++row) {
                const int i=TitleMenu::option_ids[std::size_t(menu.options_tab)][std::size_t(row)];
                const int y=66+20*row;
                m.text(names[i],{X(22),Y(y),42*ms,10*ms},menu.selection==row+1?white:gold);
                const std::string value=i==0?trim(menu.sound?bank(3944,"On"):bank(3945,"Off")):
                    i==1?com(60+menu.game_speed,speed_fallback[std::clamp(menu.game_speed,0,2)]):
                    i==2?trim(menu.instant_speech?bank(3942,"Fast"):bank(3943,"Normal")):
                    i==3?trim(menu.smooth_camera?bank(3944,"On"):bank(3945,"Off")):
                    i==4?(menu.language?tr("Russian","Русский"):tr("English","Английский")):
                    // Port row: the built-in synth, or the .sf2 next to the game when one was found.
                    i==5?(menu.soundfont && menu.soundfont_available?std::string("SoundFont"):tr("Synth","Синтез")):
                    i==6?trim(menu.smooth_motion?bank(3944,"On"):bank(3945,"Off")):
                    i==7?(menu.modern_ui && menu.wide_ui?tr("Modern","Новый"):menu.wide_ui?tr("Wide","Широкий"):std::string("Java")):
                    i==8?trim(menu.autosave?bank(3944,"On"):bank(3945,"Off")):
                    trim(menu.rumble?bank(3944,"On"):bank(3945,"Off"));
                const int value_w=width_of(value); // bh.a(..., 0): values are centred on p2+70
                m.text(value,{X(97)-value_w/2,Y(y),value_w+ms,10*ms},gold);
                sprite(0x50002,X(65),Y(y));sprite(0x50003,X(123),Y(y));
                sprite(819,X(28),Y(y+12));
            }
        } else if(page==TitlePage::reading) {
            // bl: page text from p3+45 inside 130px, page counter top right, arrows above/below.
            std::string text=model.about_text;
            if(menu.help_topic<4 && model.help_text.size()>std::size_t(menu.help_topic+6)) {
                text=model.help_text[menu.help_topic+6];
                if(menu.help_topic==0 && !model.localized) text="Switch: D-pad move, A talk/attack, X skill, ZL/ZR guardian, + menu, L use item, R change item.\n\n"+text;
            }
            const int text_w=130*ms;
            const auto wrapped=layout_bitmap_text(font,text,0,0,text_w,ms);
            int lines=1;
            for(const auto& glyph:wrapped) lines=std::max(lines,glyph.y/(int(font.height)*ms)+1);
            const int visible=std::max(1,104/int(font.height));
            const int pages=std::max(1,(lines+visible-1)/visible),current=menu.text_page%pages;
            m.text(std::to_string(current+1)+"/"+std::to_string(pages),{X(130),Y(34),24*ms,10*ms},ink);
            m.text(text,{X(23),Y(45),text_w,visible*int(font.height)*ms},gold,current*visible);
            if(current>0) m.text("^",{X(74),Y(36),12*ms,10*ms},white);
            if(current+1<pages) m.text("v",{X(74),Y(156),12*ms,10*ms},white);
        } else if(page==TitlePage::character) {
            // c: grey faces at slots 6/34/59 (y+38), the selected slot in colour; two caption lines.
            const int selected=menu.character_class-6;
            static constexpr int slot_x[3]{6,34,59};
            for(int slot=0;slot<3;++slot) touch({X(13+slot_x[slot]),Y(36),28*ms,72*ms},12,2-slot,slot==2-selected);
            for(int slot=0;slot<3;++slot) if(slot!=2-selected) sprite(710+(2-slot),X(15+slot_x[slot]),Y(10+38));
            sprite(700+selected,X(15+slot_x[2-selected]),Y(10+38));
            m.text(com(12,"CHARACTER"),{X(26),Y(114),120*ms,10*ms},gold);
            m.text(com(13,"SELECTION."),{X(26),Y(129),120*ms,10*ms},gold);
            if(menu.locked_notice) af_box(com(6,"Unlock this character")+"\n"+com(7,"after doing Ronin's story"));
        } else if(page==TitlePage::character_confirm) {
            // by: description, rule, name, colour face anchored bottom-right, YES/NO (No first).
            const int selected=menu.character_class-6;
            const auto label_text=[&](int index){return std::size_t(index)<model.hero_text.size()?model.hero_text[index]:"?";};
            m.text(label_text(15+selected),{X(21),Y(39),133*ms,40*ms},gold);
            sprite(819,X(17),Y(85));
            m.text(label_text(selected),{X(21),Y(89),120*ms,10*ms},gold);
            bottom_right(700+selected,X(135),Y(142));
            yes_no(10,5,9,37,5);
        } else if(page==TitlePage::guardians || page==TitlePage::guardian_confirm) {
            // bk: chosen guardians use frame 1 lower down, the others frame 0; the focused one
            // bobs 0..3px, the etc[20] hand points at it, etc[19] rule under the row.
            const bool confirming=page==TitlePage::guardian_confirm;
            const int ox=15,oy=10;
            sprite(819,X(ox+11),Y(oy+82));
            const int phase=int((SDL_GetTicks()/50)%6),bob=phase<=3?phase:6-phase;
            for(int i=0;i<3;++i) {
                const int cx=X(ox+22+i*34);
                if(!confirming) touch({cx-17*ms,Y(oy+28),34*ms,56*ms},12,i,menu.guardian_focus==i);
                if(menu.chosen_guardians[i]) centred(721+i*2,cx,Y(oy+61));
                else centred(720+i*2,cx,Y(oy+54+(!confirming && menu.guardian_focus==i?bob:0)));
            }
            if(!confirming) {
                sprite(820,X(ox+19+menu.guardian_focus*34),Y(oy+73));
                const int focus=menu.guardian_focus;
                if(model.guardian_text.size()>=15) {
                    m.text(model.guardian_text[focus],{X(ox+11),Y(oy+94),120*ms,10*ms},gold);
                    m.text(model.guardian_text[12+focus],{X(ox+11),Y(oy+109),100*ms,25*ms},gold);
                }
            } else {
                m.text(com(17,"ARE YOU SURE?"),{X(ox+11),Y(oy+104),120*ms,10*ms},gold);
                yes_no(ox,oy,64,92,60);
            }
        } else if(page==TitlePage::load_game) {
            // a.a: origin moves by (+10,+5); five etc[19] rules, brush behind the selected row,
            // hero names (white / 0x9f7f7f), LV icon + digits, "Progress:" line, colour face.
            const auto text_at=[&](int index,const char* fallback) {
                return model.language && std::size_t(index)<model.language->size()?(*model.language)[std::size_t(index)]:std::string(fallback);
            };
            for(int i=0;i<5;++i) sprite(819,X(23),Y(54+i*16));
            sprite(814+2*menu.brush_stage(),X(15),Y(36+menu.load_selection*16));
            for(std::size_t i=0;i<menu.saves.size() && i<5;++i)
                touch({mx,Y(38+int(i)*16),152*ms,16*ms},12,int(i),int(i)==menu.load_selection);
            for(std::size_t i=0;i<menu.saves.size();++i) {
                const int hero=menu.saves[i].hero-6;
                std::string name=std::size_t(hero)<model.hero_text.size()?model.hero_text[std::size_t(hero)]:"?";
                if(menu.saves[i].autosave) name+=tr(" (auto)"," (авто)");
                m.text(name,{X(31),Y(41+int(i)*16),110*ms,10*ms},
                       int(i)==menu.load_selection?white:SDL_Color{159,127,127,255});
            }
            if(const auto* entry=menu.selected_save()) {
                sprite(0x5000b,X(25),Y(109));
                const auto strip=textures.find(0x50005);
                const std::string level=std::to_string(std::max(0,entry->level));
                for(std::size_t i=0;i<level.size();++i) {
                    if(strip==textures.end()) break;
                    SDL_Rect src{(level[i]-'0')*5,0,5,7},dst{X(40)+int(i)*4*ms,Y(109),5*ms,7*ms};
                    SDL_RenderCopy(renderer,strip->second,&src,&dst);
                }
                std::string progress=text_at(3903,"Progress:");
                if(!progress.empty() && progress.back()!=':' && progress.back()!=' ') progress+=": ";
                m.text(progress+std::to_string(entry->progress)+"%",
                       {X(25),Y(122),110*ms,10*ms},{127,95,95,255});
                sprite(700+entry->hero-6,X(93),Y(94));
            }
        } else if(page==TitlePage::overwrite_warning) {
            // bf -> a(12, ...): language text 3929 before a new game replaces saved data.
            std::string text=model.language && model.language->size()>3929?(*model.language)[3929]:
                "Data will be overwritten.";
            std::replace(text.begin(),text.end(),';','\n');
            m.text(text,{X(23),Y(45),110*ms,100*ms},gold);
        }
        if(!menu.message.empty()) {
            const std::string& msg=menu.message;
            const std::string shown=msg=="No saved game."?tr("No saved game.","Нет сохранений."):
                msg=="Choose exactly two guardians."?tr("Choose exactly two guardians.","Выберите двух хранителей."):
                msg=="This option is not restored yet."?tr("This option is not restored yet.","Пока не перенесено."):std::string();
            if(!shown.empty()) m.text(shown,{mx-25*ms,my+174*ms,200*ms,25*ms},gold);
            else latin.text(msg,{mx-25*ms,my+174*ms,200*ms,25*ms},gold);
        }
        key_label("A",bank(3908,"Ok"),{2*ms,output_h-10*ms,80*ms,10*ms},gold);
        key_label("B",bank(3909,"Back"),{output_w-58*ms,output_h-10*ms,58*ms,10*ms},gold);
        return;
    }
    if(!model.world_hidden && !menu_screen) {
        // as.a: bottom uifrm pieces at native pixel scale; bars use actual ao HP/SP.
        const int hs=std::max(1,std::min(output_w/320,output_h/240));
        Painter hud{renderer,textures,font_texture,font,hs};
        if(!model.hud_elsewhere) draw_world_hud(renderer,model,textures,font_texture,font,output_w,output_h);
        const int reveal=location_banner_height(model.location_step);
        if(reveal && !model.location_name.empty()) {
            const int by=(output_h-21*hs)/4;
            // ae.draw clips 8/16px windows at band offsets 8/4, both centred on row 12.
            SDL_Rect clip{0,by+(reveal<22?(24-reveal)/2:0)*hs,output_w,reveal*hs};
            SDL_RenderSetClipRect(renderer,&clip);
            hud.rect({0,by,output_w,22*hs},{0,0,0,255});
            hud.rect({0,by,output_w,hs},muted);
            hud.rect({0,by+21*hs,output_w,hs},muted);
            int tw=0;
            for(const auto& glyph:layout_bitmap_text(font,model.location_name,0,0,output_w,hs))
                tw=std::max(tw,glyph.x+int(glyph.source_width)*hs);
            // Painter restores this reveal clip after rendering the text.
            hud.text(model.location_name,{(output_w-tw)/2,by+8*hs,tw+hs,10*hs},white);
            SDL_RenderSetClipRect(renderer,nullptr);
        }
    }
    if(!model.enemy_name.empty() && !model.world_hidden &&
       (model.screen==VisualScreen::none || model.screen==VisualScreen::dialogue)) {
        const int es=std::max(1,std::min(output_w/320,output_h/240));
        // as.a: 20px lower while the guardian channel box (p.i == 2) occupies the top.
        const int x=output_w-105*es,y=(model.guardian_channel?22:2)*es;
        if(!model.hud_elsewhere) draw_enemy_panel(renderer,model,textures,font_texture,font,x,y,es);
    }
    if(model.minimap && !model.world_hidden) {
        const auto& map=*model.minimap;
        const int ms=std::max(1,std::min(output_w/320,output_h/240));
        int cell=2*ms;
#if HEROES_LORE_SWITCH
        // Switch: cells 50% larger, as long as the grid still fits below the name band.
        cell=3*ms;
        if(map.width>0 && map.height>0)
            cell=std::max(2*ms,std::min(cell,std::min((output_w-8*ms)/map.width,(output_h-28*ms)/map.height)));
#endif
        Painter mini{renderer,textures,font_texture,font,ms};
        const int w=map.width*cell,h=map.height*cell,x0=output_w/2-w/2;
#if HEROES_LORE_SWITCH
        const int y0=std::max(output_h/2-h/2,20*ms+ms);
#else
        const int y0=output_h/2-h/2;
#endif
        const auto rgb=[](std::uint32_t c){return SDL_Color{Uint8(c>>16),Uint8(c>>8),Uint8(c),255};};
        // drawRect(x-1, y-1, w+1, h+1): a one-pixel black outline around the grid.
        mini.rect({x0-ms,y0-ms,w+2*ms,ms},{0,0,0,255});
        mini.rect({x0-ms,y0+h,w+2*ms,ms},{0,0,0,255});
        mini.rect({x0-ms,y0,ms,h},{0,0,0,255});
        mini.rect({x0+w,y0,ms,h},{0,0,0,255});
        for(int y=0;y<map.height;++y)
            for(int x=0;x<map.width;++x) {
                const auto i=std::size_t(y)*map.width+x;
                if(i<map.cells.size()) mini.rect({x0+x*cell,y0+y*cell,cell,cell},rgb(map.cells[i]));
            }
        if(map.hero_lit) mini.rect({x0+map.hero_x*cell,y0+map.hero_y*cell,cell,cell},rgb(0xFF3F3F));
        // Black 20px band across the top with the map name centred at y=8.
        mini.rect({0,0,output_w,20*ms},{0,0,0,255});
        if(!model.location_name.empty()) {
            int tw=0;
            for(const auto& glyph:layout_bitmap_text(font,model.location_name,0,0,output_w,ms))
                tw=std::max(tw,glyph.x+int(glyph.source_width)*ms);
            mini.text(model.location_name,{(output_w-tw)/2,8*ms,tw+ms,10*ms},{255,255,255,255});
        }
    }
    if (model.screen==VisualScreen::none) {
        if(!model.notice.empty() && !model.world_hidden) {
            // as.a message: cb.a(...,false) frame, 0x5F1F3F box, white text centred (port: wider box above the HUD).
            // Small screens (960x544 Vita, 640x480 handhelds) take the HUD scale up to 2: the 640-wide UI
            // scale is 1 there and the text came out tiny.
            const int ts=std::max(s,std::min({output_w/320,output_h/240,2}));
            Painter t{renderer,textures,font_texture,font,ts};
            SDL_Rect toast{output_w/2-180*ts,output_h-36*std::max(1,std::min(output_w/320,output_h/240))-30*ts,360*ts,24*ts};
            cb_frame(t,toast.x/ts,toast.y/ts,toast.w/ts,toast.h/ts);
            t.rect({toast.x+2*ts,toast.y+2*ts,toast.w-4*ts,toast.h-4*ts},{95,31,63,255});
            int notice_w=0;
            for(const auto& g:layout_bitmap_text(font,model.notice,0,0,100000,ts)) notice_w=std::max(notice_w,g.x+int(g.source_width)*ts);
            notice_w=std::min(notice_w,toast.w-12*ts);
            t.line(model.notice,{toast.x+(toast.w-notice_w)/2,toast.y+6*ts,notice_w+ts,12*ts},{255,255,255,255});
        }
        return;
    }
    // 3DS: dialogue, Yes/No, item messages and event notices go to the bottom screen (draw_bottom_screen).
    if(model.hud_elsewhere && !model.bottom_pass && (model.screen==VisualScreen::dialogue ||
       model.screen==VisualScreen::choice || model.screen==VisualScreen::notice)) return;
    if (model.screen==VisualScreen::dialogue && !model.item_box) {
        const auto layout=calculate_dialogue_layout(output_w,output_h,font.height,model.portrait);
        Painter d{renderer,textures,font_texture,font,layout.scale};
        const auto& a=layout.panel; SDL_Rect box{a.x,a.y,a.w,a.h}; d.frame(box,false,{0,0,0,255});
        if (model.portrait) {
            const auto found=textures.find(100+std::abs(model.portrait)-1);
            if (found!=textures.end()) {
                const int ps=layout.scale;
                const int px=model.portrait>0?box.x+8*ps:box.x+box.w-58*ps;
                // ah.b: portrait bottom anchored above the dialogue, without a card.
                SDL_Rect dst{px,box.y-60*ps,50*ps,60*ps};
                SDL_RenderCopy(renderer,found->second,nullptr,&dst);
            }
        }
        d.text(model.text,{layout.inner.x,layout.inner.y,layout.inner.w,layout.inner.h},gold,model.scroll_lines,
               model.dialogue_visible_end);
        const int ds=layout.scale;
        Painter dl{renderer,textures,model.latin_font_texture?model.latin_font_texture:font_texture,
                   model.latin_font?*model.latin_font:font,ds};
        // Port: dark plates keep Next/Skip readable over the uifrm HUD strip (3DS: on the bottom screen).
        const int hint_y=output_h-int(font.height)*ds-2*ds;
        if(!model.hud_elsewhere) {
            draw_key_hints(d,dl,{5*ds,hint_y,120*ds,int(font.height)*ds},gold,
                           {{"A",bank_text(model,3911,"Next")}},true);
            draw_key_hints(d,dl,{output_w-125*ds,hint_y,120*ds,int(font.height)*ds},gold,
                           {{"B",bank_text(model,3910,"Skip")}},true,true);
        }
        return;
    }
    if(model.screen==VisualScreen::choice || (model.screen==VisualScreen::dialogue && model.item_box)) {
        // ah.c(Graphics) YES/NO: cb box 70% of the 240px screen wide, 60 high, centred 30 above the
        // middle; question, then Yes/No with the ce.d marker. ah state 4: 120x45 message box.
        const int ms=std::max(1,std::min(output_w/240,output_h/220));
        const int lw=output_w/ms,lh=output_h/ms;
        Painter m{renderer,textures,font_texture,font,ms};
        Painter latin{renderer,textures,model.latin_font_texture?model.latin_font_texture:font_texture,
                      model.latin_font?*model.latin_font:font,ms};
        const auto R=[&](int x,int y,int w,int h,unsigned c) {
            if(w>0 && h>0) m.rect({x*ms,y*ms,w*ms,h*ms},{Uint8(c>>16),Uint8(c>>8),Uint8(c),255});
        };
        const auto box=[&](int x,int y,int w,int h) { // cb.a(g,x,y,w,h) + cb.b(g,x,y,w,h)
            R(x+1,y,w-2,1,0x1f1f3f);R(x+w-1,y+1,1,h-2,0x1f1f3f);R(x+1,y+h-1,w-2,1,0x1f1f3f);R(x,y+1,1,h-2,0x1f1f3f);
            R(x+1,y+1,w-3,1,0x5f3f3f);R(x+1,y+1,1,h-3,0x5f3f3f);R(x+w-2,y+1,1,h-3,0x1f1f3f);R(x+1,y+h-2,w-2,1,0x1f1f3f);
            R(x+2,y+2,w-4,h-4,0x3f1f3f);
        };
        const auto height_of=[&](const std::string& text,int width) {
            int h=int(font.height);
            for(const auto& g:layout_bitmap_text(font,text,0,0,width,1)) h=std::max(h,g.y+int(font.height));
            return h;
        };
        const SDL_Color W{255,255,255,255};
        if(model.screen==VisualScreen::dialogue) {
            const int x=lw/2-60,y=lh/2-25,h=std::max(45,height_of(model.text,108)+14);
            box(x,y,120,h);
            m.text(model.text,{(x+6)*ms,(y+7)*ms,108*ms,(h-9)*ms},W);
        } else {
            const auto com_text=[&](std::size_t i,const char* fallback){
                return i<model.common_text.size() && !model.common_text[i].empty()?model.common_text[i]:std::string(fallback);};
            const auto bank_text=[&](std::size_t i,const char* fallback){
                return model.language && i<model.language->size()?(*model.language)[i]:std::string(fallback);};
            const int w=240*70/100,x=lw/2-w/2,question=height_of(model.text,w-12);
            const int h=std::max(60,6+question+5+2*(int(font.height)+3)+4),y=lh/2-30;
            box(x,y,w,h);
            int ty=y+6;
            m.text(model.text,{(x+6)*ms,ty*ms,(w-12)*ms,question*ms},W);
            ty+=question+5;
            const std::string options[2]={bank_text(3915,com_text(0,"Yes").c_str()),bank_text(3916,com_text(1,"No").c_str())};
            for(int i=0;i<2;++i) {
                if((i==0)==model.choice_yes) if(const auto arrow=textures.find(0x50003);arrow!=textures.end()) {
                    int iw=0,ih=0;SDL_QueryTexture(arrow->second,nullptr,nullptr,&iw,&ih);
                    SDL_Rect dst{(x+6)*ms,ty*ms,iw*ms,ih*ms};SDL_RenderCopy(renderer,arrow->second,nullptr,&dst);
                }
                m.text(options[i],{(x+15)*ms,ty*ms,(w-21)*ms,(int(font.height)+1)*ms},W);
                ty+=int(font.height)+3;
            }
        }
        const SDL_Color gold{255,159,0,255};
        {const int kw=std::max(key_cap(latin,"A",2*ms,output_h-12*ms,10*ms,gold,true)+3*ms,14*ms);
        m.text(model.language && model.language->size()>3908?(*model.language)[3908]:std::string("Ok"),{2*ms+kw,output_h-12*ms,90*ms,10*ms},gold);}
        return;
    }
    if (model.screen==VisualScreen::choice) {
        // Original choice is a compact overlay; the world remains visible.
        const int cw=std::min(output_w-24*s,280*s);
        const auto lines=layout_bitmap_text(font,model.text,0,0,cw-20*s,s);
        int text_height=int(font.height)*s;
        for(const auto& glyph:lines) text_height=std::max(text_height,glyph.y+int(font.height)*s);
        const int ch=std::min(output_h-24*s,text_height+52*s);
        SDL_Rect box{(output_w-cw)/2,(output_h-ch)/2,cw,ch};
        p.rect(box,ink);p.outline(box,muted,s);
        p.text(model.text,{box.x+8*s,box.y+8*s,box.w-16*s,ch-44*s},gold);
        const int row=std::max(12,int(font.height)+3)*s;
        const int cy=box.y+box.h-2*row-5*s;
        const auto com_text=[&](std::size_t i,const char* fallback){
            return i<model.common_text.size() && !model.common_text[i].empty()?model.common_text[i]:std::string(fallback);};
        p.text(com_text(0,"Yes"),{box.x+24*s,cy,box.w-32*s,row},gold);
        p.text(com_text(1,"No"),{box.x+24*s,cy+row,box.w-32*s,row},gold);
        const int ay=cy+(model.choice_yes?0:row)+3*s;
        for(int i=0;i<5*s;++i) p.rect({box.x+10*s+i,ay+i/2,1,5*s-i},white);
        return;
    }
    if(model.screen==VisualScreen::game_menu || model.screen==VisualScreen::shop ||
       (model.screen==VisualScreen::service && model.service)) {
        // The shop (bp, 155x170, six sgui/shop tabs) and the blacksmith / refiner (at, ch, ap, k)
        // share the ai/cb primitives.
        const bool shop=model.screen==VisualScreen::shop;
        const bool service_ui=model.screen==VisualScreen::service;
        // ai: 155x176 0x3f1f3f box centred at (r.i-77, r.j-85), cb.c panel, six gmico tabs.
        const int ms=std::max(1,std::min(output_w/240,output_h/220));
        Painter m{renderer,textures,font_texture,font,ms};
        Painter latin{renderer,textures,model.latin_font_texture?model.latin_font_texture:font_texture,
                      model.latin_font?*model.latin_font:font,ms};
        // Port wide layout: the box grows by ex. Past the split column x moves right by ex, so the
        // left column (tabs, slots) stays, the detail side widens and right-aligned numbers follow
        // the right edge. The split is off while the tab strip draws; plain() draws unmapped.
        const int wide_ex=model.wide_ui?std::clamp(output_w/ms-155-24,0,140):0;
        int ex=wide_ex;
        int split=1<<20;
        const auto wx=[&](int x){return x<split?x:x+ex;};
        const auto plain=[&](const auto& draw){const int saved=split;split=1<<20;draw();split=saved;};
        // Port Modern layout: one MW x MH frame with a tab column, named lists and a detail panel.
        const bool modern=model.modern_ui && !shop && !service_ui;
        const bool modern_shop=model.modern_ui && shop;
        const int MW=std::clamp(output_w/ms-16,240,420),MH=std::clamp(output_h/ms-28,176,220);
        const int frame_x=output_w/2-MW/2*ms,frame_y=std::max(0,output_h/2-(MH/2+11)*ms); // room for two hint lines below
        int cx=modern?frame_x:output_w/2-(77+ex/2)*ms,cy=modern?frame_y:output_h/2-85*ms;
        const int tab=std::clamp(shop?model.category:model.menu_tab,0,5);
        const auto C=[](unsigned rgb){return SDL_Color{Uint8(rgb>>16),Uint8(rgb>>8),Uint8(rgb),255};};
        const SDL_Color W=C(0xffffff),T=C(0xdfbf7f);
        // All primitives take original pixels relative to the box origin.
        const auto R=[&](int x,int y,int w,int h,unsigned c){
            if(w>0 && h>0) {const int x1=wx(x);m.rect({cx+x1*ms,cy+y*ms,(wx(x+w)-x1)*ms,h*ms},C(c));}};
        const auto lines=[&](int x,int y,int w,int h,unsigned c1,unsigned c2) { // cb.a (private)
            R(x+1,y,w-2,1,c1);R(x,y+1,1,h-2,c1);R(x+w-1,y+1,1,h-1,c2);R(x+1,y+h-1,w-2,1,c2);
        };
        const auto bevel=[&](int x,int y,int w,int h,unsigned c1,unsigned c2,unsigned c3,unsigned fill) {
            R(x+1,y,w-2,1,c1);R(x+w-1,y+1,1,h-2,c1);R(x+1,y+h-1,w-2,1,c1);R(x,y+1,1,h-2,c1);
            R(x+1,y+1,w-3,1,c2);R(x+1,y+1,1,h-3,c2);R(x+w-2,y+1,1,h-3,c3);R(x+1,y+h-2,w-2,1,c3);
            R(x+2,y+2,w-4,h-4,fill);
        };
        const auto rounded=[&](int x,int y,int w,int h,unsigned c) { // cb.b(..., colour)
            R(x+1,y,w-2,1,c);R(x,y+1,1,h-2,c);R(x+w-1,y+1,1,h-2,c);R(x+1,y+h-1,w-2,1,c);R(x+1,y+1,w-2,h-2,c);
        };
        const auto texture_size=[&](int key){SDL_Point size{};const auto f=textures.find(key);
            if(f!=textures.end()) {SDL_QueryTexture(f->second,nullptr,nullptr,&size.x,&size.y);}
            return size;};
        // anchor: 20 top-left, 3 centre, 24 top-right, 36 bottom-left.
        const auto image=[&](int key,int x,int y,int anchor=20) {
            const auto f=textures.find(key);if(f==textures.end()) return;
            const auto size=texture_size(key);x=wx(x);
            if(anchor==3) {x-=size.x/2;y-=size.y/2;} else if(anchor==24) x-=size.x; else if(anchor==36) y-=size.y;
            else if(anchor==33) {x-=size.x/2;y-=size.y;}
            SDL_Rect dst{cx+x*ms,cy+y*ms,size.x*ms,size.y*ms};SDL_RenderCopy(renderer,f->second,nullptr,&dst);
        };
        const auto clean=[](std::string text){text.erase(std::remove(text.begin(),text.end(),'|'),text.end());return text;};
        const auto text_width=[&](const std::string& text){int w=0;
            for(const auto& g:layout_bitmap_text(font,text,0,0,1000,1)) {w=std::max(w,g.x+int(g.source_width));}
            return w;};
        const auto line_count=[&](const std::string& text,int width){int count=1;
            for(const auto& g:layout_bitmap_text(font,text,0,0,width,1)) {count=std::max(count,g.y/int(font.height)+1);}
            return count;};
        const auto say=[&](const std::string& text,int x,int y,SDL_Color c,int width=150,int height=0) {
            const int x1=wx(x);
            m.text(clean(text),{cx+x1*ms,cy+y*ms,(wx(x+width)-x1)*ms,(height?height:int(font.height)+1)*ms},c);
        };
        // Port: text taller than its box pages with X (detail_page); the hint shows only then.
        bool text_pages=false;
        const auto say_paged=[&](const std::string& text,int x,int y,SDL_Color c,int width,int height) {
            const std::string t=clean(text);
            const int x1=wx(x);width=wx(x+width)-x1;
            const int lines=line_count(t,width);
            if(lines*int(font.height)<=height) {m.text(t,{cx+x1*ms,cy+y*ms,width*ms,height*ms},c);return;}
            text_pages=true;
            // The last line of the box carries the "1/2" page marker.
            const int shown=std::max(1,height/int(font.height)-1);
            const int pages=(lines+shown-1)/shown,page=model.detail_page%pages;
            m.text(t,{cx+x1*ms,cy+y*ms,width*ms,shown*int(font.height)*ms},c,page*shown);
            const std::string marker=std::to_string(page+1)+"/"+std::to_string(pages);
            m.text(marker,{cx+(x1+width-text_width(marker))*ms,cy+(y+shown*int(font.height))*ms,
                           (text_width(marker)+2)*ms,(int(font.height)+1)*ms},W);
        };
        // r.c variant 0: 5px digit cells advancing 4px; anchor 4 left / 8 right.
        const auto digits=[&](int value,int x,int y,bool right) {
            const std::string text=std::to_string(std::max(0,value));
            const int width=int(text.size())*4;x=right?wx(x)-width:wx(x);
            const auto strip=textures.find(0x50005);
            if(strip==textures.end()) {m.text(text,{cx+x*ms,cy+y*ms,(width+2)*ms,(int(font.height)+1)*ms},W);return width;}
            for(std::size_t i=0;i<text.size();++i) {
                SDL_Rect src{(text[i]-'0')*5,0,5,7},dst{cx+(x+int(i)*4)*ms,cy+y*ms,5*ms,7*ms};
                SDL_RenderCopy(renderer,strip->second,&src,&dst);
            }
            return width;
        };
        // Grouped numbers anchor once at their right edge so the wide split never pulls them apart.
        const auto ratio=[&](int x,int y,int current,int maximum) { // r.d
            x=wx(x);
            plain([&]{
                const int width=digits(maximum,x,y,true);
                image(0x5000e,x-width-texture_size(0x5000e).x,y);
                digits(current,x-width-9,y,true);
            });
        };
        const auto label=[&](const std::string& text,int x,int y) { // r.a: white text on a black box
            const std::string t=clean(text);const int x1=wx(x);
            m.rect({cx+(x1-1)*ms,cy+(y-1)*ms,(text_width(t)+2)*ms,(int(font.height)+2)*ms},C(0));
            m.text(t,{cx+x1*ms,cy+y*ms,(text_width(t)+2)*ms,(int(font.height)+1)*ms},W);
        };
        const auto gm=[&](int i){return std::size_t(i)<model.menu_text.size()?model.menu_text[std::size_t(i)]:std::string();};
        const auto com=[&](int i){return std::size_t(i)<model.common_text.size()?model.common_text[std::size_t(i)]:std::string();};
        // Like the shop, services hide the HUD; the port shows the blurred field behind them.
        if(model.bottom_pass) draw_bottom_backdrop(renderer,textures,font_texture,font); // 3DS: no field behind
        else {
            m.rect({0,0,output_w,output_h},{0,0,0,255});
            if(menu_backdrop) SDL_RenderCopy(renderer,menu_backdrop,nullptr,nullptr);
        }
        if(!service_ui && !modern && !modern_shop) {
            const int box_h=shop?170:176,tab_x=shop?11:5;
            R(0,0,155+ex,box_h,0x3f1f3f);
            lines(2,15,151+ex,box_h-16,0xffdfbf,0xbf9f7f);R(3,16,149+ex,box_h-18,0xdfbf7f);
            R(tab_x+tab*16+1,0,14,1,0xffdfbf);R(tab_x+tab*16,1,1,16,0xffdfbf);
            R(tab_x+tab*16+15,1,1,15,0xbf9f7f);R(tab_x+tab*16+1,1,14,16,0xdfbf7f);
            for(int i=0;i<6;++i) {
                image(shop?0x51000+i:300+(i==5?6:i),tab_x+2+16*i,1);
                g_ui_hits.push_back({{cx+(tab_x+16*i)*ms,cy,16*ms,17*ms},1,i,i==tab,false});
            }
            if(shop) {image(0x50002,4,4);image(0x50003,109,4);}
        }
        split=100;
        // Tab content origin (p2+2, p3+15); m lists move it to (p2+2, p3+11).
        int bx=2,by=15;
        const auto slot=[&](int index,bool selected) { // cb.a(g,x,y,byte,boolean)
            const int x=bx+3,y=by+10+index*23;
            const unsigned outer=selected?0x3f1f3f:0x5f3f3f,light=selected?0x9f7f3f:0xdfbf7f;
            R(x+1,y,24,1,outer);R(x,y+1,1,16,outer);R(x+1,y+17,24,1,outer);
            R(x+1,y+1,24,1,light);R(x+1,y+1,1,16,light);
            R(x+2,y+16,23,1,selected?0x3f3f3f:0x7f5f7f);
            R(x+2,y+2,24,14,selected?0x5f3f3f:0x9f7f7f);
        };
        const auto list=[&](int count,int selection,bool arrows) { // cb.a(g,x,y,boolean)
            const int page=selection/5,shown=std::clamp(count-page*5,0,5);
            for(int i=0;i<shown;++i) // the slot and its row across the panel
                g_ui_hits.push_back({{cx+(bx+3)*ms,cy+(by+10+i*23)*ms,(wx(bx+147)-(bx+3))*ms,18*ms},2,page*5+i,i==selection%5,false});
            for(int i=0;i<shown;++i) if(i!=selection%5) slot(i,false);
            bevel(bx+27,by+10,120,137,0x3f1f3f,0x9f7f3f,0x3f3f3f,0x5f3f3f);
            slot(selection%5,true);
            if(arrows) {
                plain([&]{
                    if(page>0) image(0x50000,bx+70+ex/2,by+4);
                    if((page+1)*5<count) image(0x50001,bx+70+ex/2,by+148);
                });
            }
        };
        const auto money=[&](){ // cb.a(g,x,y,int): digits, coin to their left
            const int x=wx(bx+110);
            plain([&]{
                const int width=digits(int(std::min<std::int64_t>(model.gold,999999999)),x,by+2,true);
                image(0x50007,x-width,by+2,24);
            });
        };
        const auto hero_label=[&](int index) -> std::string {
            return index>=0 && std::size_t(index)<model.hero_text.size()?model.hero_text[index]:"?";
        };
        // cb.a(g, x, y, ad): item details at (33, 14) of the tab area.
        const auto item_detail=[&](const VisualItem& item) {
            const int x=bx+33;int y=by+14;
            const int line=int(font.height)+2;
            if(item.power>=0 && !item.identified) {
                say(item.name,x,y,T,115,line*4);y+=(line_count(item.name,115+ex)-1)*line;
                say(com(5),x,y+16,W,115);
            } else if(item.power>=0) {
                // Equipment: name, red option, Attacking/Defense value, Refining e/d, bonuses + description.
                say(item.name,x,y,W,115,line*4);y+=(line_count(item.name,115+ex)-1)*line;
                if(!item.option_name.empty()) say(item.option_name,x+55,y+10,C(0xff0000),60);
                say(com(item.weapon?4:46),x,y+25,T,80);digits(item.power,x+108,y+25,true);
                say(com(3),x,y+40,T,80);ratio(x+108,y+40,item.refine,item.refine_max);
                say_paged(item.description,x,y+55,T,110,std::max(10,by+145-(y+55)));
            } else {
                say(item.name,x,y,W,115,line*4);y+=(line_count(item.name,115+ex)-1)*line;
                say_paged(item.description,x,y+15,T,110,std::max(10,by+145-(y+15)));
            }
        };
        const auto st=[&](int i){return model.shop_text && std::size_t(i)<model.shop_text->size()?(*model.shop_text)[std::size_t(i)]:std::string();};
        const auto money_at=[&](std::int64_t value,int x,int y) { // cb.a(g,x,y,int)
            x=wx(x);
            plain([&]{
                const int width=digits(int(std::clamp<std::int64_t>(value,0,999999999)),x,y,true);
                image(0x50007,x-width,y,24);
            });
        };
        // q: one Status page's content at (bx+34, by+18); shared with the Modern detail panel.
        const auto status_page_detail=[&]{
            if(model.status_page==0) {
                say(hero_label(model.character_class-6),bx+35,by+18,W);
                say(hero_label(model.character_class-3),bx+33,by+35,T);
                image(0x5000b,bx+35,by+52);digits(model.progress.level,bx+52,by+52,false);
                image(0x50009,bx+34,by+70);digits(model.progress.experience,bx+102,by+70,true);
                const int next=std::max(1,model.progress.next_experience());
                R(bx+34,by+79,72,3,0x3f1f3f);
                R(bx+35,by+80,70*std::min(model.progress.experience,next)/next,1,0xffffff);
                image(0x5000c,bx+38,by+84);digits(next,bx+102,by+84,true);
                image(0x5000a,bx+34,by+97);ratio(bx+102,by+96,model.vitals.hp,model.vitals.max_hp);
                image(0x5000f,bx+34,by+106);ratio(bx+102,by+105,model.vitals.sp,model.vitals.max_sp);
            } else if(model.status_page==1) {
                for(int i=0;i<6;++i) {
                    say(hero_label(9+i),bx+38,by+21+15*i,T,60);
                    digits(i<4?model.progress.attributes[i]+model.progress.gear_bonus[i]:i==4?model.attack:model.defense,bx+100,by+22+15*i,true); // q: base + gear
                }
            } else if(model.status_page==2) {
                say(hero_label(model.character_class-6),bx+34,by+18,T);
                say_paged(hero_label(model.character_class),bx+34,by+30,W,110,110);
            } else if(model.status_page==3) {
                rounded(bx+34,by+22,100,26,0x3f1f3f);
                say(gm(3),bx+37,by+25,W,95);say(gm(4),bx+37,by+36,W,60);
                digits(model.progress.attribute_points,bx+99,by+36,true);
                rounded(bx+34,by+62,100,33,0x3f1f3f);
                say(gm(5),bx+40,by+72,W,20);say(gm(6),bx+60,by+67,T,72);say(gm(7),bx+60,by+80,T,72);
            }
        };
        // Modern helpers, in frame pixels: frame, tab column, named list, panels and a widened detail origin.
        struct ModernRow {int icon{-1};std::string name;int count{};bool mark{};std::int64_t price{-1};};
        const int m_top=8,m_body=MH-16;
        const auto modern_frame=[&]{
            ex=0;split=1<<20;cx=frame_x;cy=frame_y;
            R(0,0,MW,MH,0x3f1f3f);lines(2,2,MW-4,MH-4,0xffdfbf,0xbf9f7f);R(3,3,MW-6,MH-6,0xdfbf7f);
        };
        // One line only: a longer name loses whole UTF-8 characters and ends in "..".
        const auto fit=[&](const std::string& text,int width) {
            std::string t=clean(text);
            if(text_width(t)<=width) return t;
            while(!t.empty() && text_width(t+"..")>width) {
                do t.pop_back(); while(!t.empty() && (static_cast<unsigned char>(t.back())&0xC0)==0x80);
                if(!t.empty() && static_cast<unsigned char>(t.back())>=0xC0) t.pop_back();
            }
            while(!t.empty() && t.back()==' ') t.pop_back();
            return t+"..";
        };
        // A platform key name (ZL, LT, 1...) as a small cap in the port's 3x5 letters; returns its width.
        const auto key_microcap=[&](const std::string& key,int x,int y) {
            if(playstation_shape(key)>=0) {
                key_cap(m,key,cx+(x+1)*ms,cy+(y+1)*ms,7*ms,W,true);
                return 10;
            }
            const int w=micro_text_width(key)+4;
            m.rect({cx+x*ms,cy+y*ms,w*ms,9*ms},{31,15,31,255});m.outline({cx+x*ms,cy+y*ms,w*ms,9*ms},W,ms);
            draw_micro_text(renderer,key,cx+(x+2)*ms,cy+(y+2)*ms,ms,W);
            return w;
        };
        // Tab column: icon plus a name of up to ~62px; a narrow frame (640x480 handhelds) keeps the icons only.
        const bool m_icon_tabs=MW<360;
        const int m_tabs_w=m_icon_tabs?26:86;
        const auto modern_tabs=[&](const std::vector<std::pair<int,std::string>>& tabs,int selected) {
            plain([&]{
                for(int i=0;i<int(tabs.size());++i) {
                    const int y=8+i*22;const bool on=i==selected;
                    g_ui_hits.push_back({{cx+6*ms,cy+y*ms,(m_tabs_w-6)*ms,20*ms},1,i,on,true});
                    // The column holding the cursor shows its pick in the dark selected colour.
                    if(on) bevel(6,y,m_tabs_w-6,20,0x3f1f3f,0x9f7f3f,0x3f3f3f,model.tab_focus?0x3f1f3f:0x5f3f3f);
                    image(tabs[std::size_t(i)].first,9,y+3);
                    if(!m_icon_tabs) say(fit(tabs[std::size_t(i)].second,m_tabs_w-30),27,y+6,on?W:C(0x5f3f3f),m_tabs_w-28);
                }
                if(!m_icon_tabs) money_at(model.gold,m_tabs_w,MH-17);
            });
        };
        const auto modern_panel=[&](int x,int y,int w,int h){plain([&]{bevel(x,y,w,h,0x3f1f3f,0x9f7f3f,0x3f3f3f,0x5f3f3f);});};
        // Rows of icon, name, count or price; the window keeps the selection in view.
        const auto modern_list=[&](const std::vector<ModernRow>& rows,int selection,int lx,int top,int lw,int h,const std::string& empty) {
            const int row_h=19,visible=std::max(1,(h-6)/row_h);
            const int count=int(rows.size()),selected=std::min(selection,std::max(0,count-1));
            const int first=std::clamp(selected-visible/2,0,std::max(0,count-visible));
            modern_panel(lx,top,lw,h);
            plain([&]{
                if(rows.empty()) say(empty,lx+6,top+6,W,lw-12,40);
                for(int i=first;i<count && i<first+visible;++i) {
                    const int y=top+3+(i-first)*row_h;const bool on=i==selected;const auto& row=rows[std::size_t(i)];
                    g_ui_hits.push_back({{cx+(lx+3)*ms,cy+y*ms,(lw-6)*ms,(row_h-1)*ms},2,i,on,true});
                    // While the tab column holds the cursor the list pick fades to the panel's lighter tone.
                    if(on) rounded(lx+3,y,lw-6,row_h-1,model.tab_focus?0x7f5f5f:0x3f1f3f);
                    if(row.icon>=0) image(row.icon,lx+13,y+9,3);
                    const int right=row.price>=0?52:row.count>1?18:row.mark?14:6;
                    say(fit(row.name,lw-25-right),lx+25,y+5,on?W:T,lw-25-right+2);
                    if(row.price>=0) money_at(row.price,lx+lw-6,y+6);
                    else if(row.count>1) digits(row.count,lx+lw-6,y+6,true);
                    if(row.mark) image(0x50006,lx+lw-10,y+13,36);
                }
                if(first>0) image(0x50000,lx+lw/2-3,top-5);
                if(first+visible<count) image(0x50001,lx+lw/2-3,top+h-2);
            });
        };
        // The original detail code draws at (bx+ox, by+oy): put that inside the panel and fit it to dw
        // (a negative ex narrows it, so numbers stay inside a small panel).
        const auto modern_detail=[&](int dx,int dw,int ox,int oy){ex=std::max(-50,dw-125);bx=dx+4-ox;by=m_top+4-oy;split=bx+100;};
        bool modern_done=false;
        if(modern) {
            modern_frame();
            modern_tabs({{300,gm(2)},{301,com(2)},{302,gm(20)},{303,gm(30)},{304,gm(39)},{306,gm(41)}},tab);
            const int lx=m_tabs_w+6,lw=std::min(200,(MW-lx-10)*11/20),dx=lx+lw+4,dw=MW-dx-6;
            const bool list_tab=(tab==0 && !model.attribute_editing) || tab==1 || tab==2 || tab==4 || tab==5 || (tab==3 && !model.skill_screen);
            if(list_tab) {
                std::vector<ModernRow> rows;
                const auto gtext=[&](int i) -> std::string {
                    return i>=0 && std::size_t(i)<model.guardian_text.size()?model.guardian_text[i]:"?";
                };
                if(tab==1 || tab==2) {
                    const auto& items=tab==1?model.items:model.equipped;
                    for(std::size_t i=0;i<(tab==1?items.size():5);++i) {
                        ModernRow row;
                        if(i<items.size() && items[i].type>=0) {
                            row.icon=200+items[i].type;row.name=items[i].name;if(tab==1) row.count=items[i].count;
                        } else {row.icon=307+int(i);row.name="-";}
                        rows.push_back(row);
                    }
                } else if(tab==3) {
                    for(std::size_t i=0;i<5;++i) {
                        ModernRow row;
                        if(i<model.guardians.slots.size() && model.guardians.slots[i]) {
                            row.icon=500+model.guardians.slots[i]->id;row.name=gtext(model.guardians.slots[i]->id);
                            row.mark=i==model.guardians.equipped;
                        } else row.name="-";
                        rows.push_back(row);
                    }
                } else if(tab==4 && model.recipes_view) for(const auto& recipe:model.recipes) {
                    ModernRow row;row.icon=recipe.icon_type>=0?200+recipe.icon_type:-1;row.name=recipe.result;rows.push_back(row);
                } else if(tab==4) for(const auto& quest:model.quests) {ModernRow row;row.icon=218;row.name=quest.name;rows.push_back(row);}
                else if(tab==0) { // q: the four Status pages; the bi icon marks unspent points
                    for(const auto& name:{hero_label(model.character_class-6),tr_text(model,"Attributes","Атрибуты"),
                                           tr_text(model,"Profile","Описание"),gm(3)}) {ModernRow row;row.name=name;rows.push_back(row);}
                    rows[3].icon=0x50008;rows[3].mark=model.progress.attribute_points>0;
                } else for(int i=0;i<4;++i) {ModernRow row;row.name=gm(42+i);rows.push_back(row);} // d: Save/Help/Options/Quit
                const bool recipe_list=tab==4 && model.recipes_view;
                if(recipe_list) plain([&]{label(tr_text(model,"Recipes","Рецепты"),lx+1,m_top+1);});
                modern_list(rows,tab==0?model.status_page:int(model.selection),lx,recipe_list?m_top+14:m_top,lw,recipe_list?m_body-14:m_body,
                            recipe_list?tr_text(model,"No recipes yet.","Рецептов пока нет."):gm(tab==4?58:15));
                modern_panel(dx,m_top,dw,m_body);
                if(tab==0) {modern_detail(dx,dw,34,18);status_page_detail();}
                else if(tab==5) plain([&]{ // port: who and where the save will record
                    say(hero_label(model.character_class-6),dx+6,m_top+6,W,dw-12);
                    say(hero_label(model.character_class-3),dx+6,m_top+21,T,dw-12);
                    image(0x5000b,dx+6,m_top+38);digits(model.progress.level,dx+23,m_top+38,false);
                    if(!model.location_name.empty()) say(model.location_name,dx+6,m_top+54,T,dw-12);
                });
                else if(tab==1 || tab==2) {
                    modern_detail(dx,dw,33,14);
                    const auto& items=tab==1?model.items:model.equipped;
                    if(model.selection<items.size() && items[model.selection].type>=0) item_detail(items[model.selection]);
                    else if(tab==1) say(gm(15),bx+33,by+14,W,115);
                    else if(model.selection==1 && model.character_class!=8) say(gm(49),bx+33,by+14,W,115,30);
                    else say(gm(21),bx+33,by+14,W,115);
                } else if(tab==3) {
                    modern_detail(dx,dw,34,18);
                    const auto& chosen=model.guardians.slots[std::min<std::size_t>(4,model.selection)];
                    if(!chosen) say(gm(31),bx+34,by+18,T);
                    else {
                        const auto& g=*chosen;
                        say(gtext(g.id),bx+34,by+18,W,55);
                        if(model.selection==model.guardians.equipped) plain([&]{label(gm(32),bx+89+ex,by+22);});
                        say(gtext(g.id+6),bx+34,by+35,T,110);
                        image(0x5000b,bx+34,by+53);digits(g.level,bx+50,by+53,false);
                        image(0x50009,bx+34,by+67);digits(g.experience,bx+102,by+67,true);
                        const int next=std::max(1,g.next_experience());
                        R(bx+34,by+76,72,3,0x3f1f3f);R(bx+35,by+77,70*std::min(g.experience,next)/next,1,0xffffff);
                        image(0x5000c,bx+38,by+81);digits(next,bx+102,by+81,true);
                    }
                } else if(recipe_list) {
                    if(!model.recipes.empty()) plain([&]{ // the result, then each part with the count carried
                        const auto& recipe=model.recipes[std::min(model.selection,model.recipes.size()-1)];
                        say(recipe.result,dx+6,m_top+6,W,dw-12);
                        int y=m_top+24;
                        for(const auto& part:recipe.parts) {
                            say("+ "+fit(part.first,dw-40),dx+6,y,part.second>0?W:T,dw-36);
                            if(part.second>0) digits(part.second,dx+dw-8,y+1,true);
                            y+=14;
                        }
                    });
                } else if(!model.quests.empty()) {
                    modern_detail(dx,dw,33,14);
                    const auto& quest=model.quests[std::min(model.selection,model.quests.size()-1)];
                    int y=by+14;
                    say(quest.name,bx+33,y,W,105,45);y+=line_count(quest.name,105+ex)*15;
                    say(gm(quest.main_quest?55:56),bx+33,y,T,105);y+=15;
                    say(gm(57),bx+33,y,T,105);y+=15;
                    if(!quest.location.empty()) {say(quest.location,bx+33,y,W,105);y+=15;}
                    if(!quest.giver.empty()) say(quest.giver,bx+33,y,W,105);
                    if(model.quest_reading) {
                        m.rect({0,0,output_w,output_h},{0,0,0,150});
                        SDL_Rect modal{(output_w-(220+wide_ex)*ms)/2,(output_h-154*ms)/2,(220+wide_ex)*ms,154*ms};
                        m.rect(modal,ink);m.outline(modal,muted,ms);
                        m.text(quest.name,{modal.x+6*ms,modal.y+6*ms,modal.w-12*ms,24*ms},gold);
                        m.paragraph(quest.description+"\n\n"+clean(gm(54))+"\n"+quest.reward,
                            {modal.x+6*ms,modal.y+34*ms,modal.w-12*ms,100*ms},model.detail_page);
                        draw_key_hints(m,latin,{modal.x+6*ms,modal.y+140*ms,modal.w-12*ms,10*ms},gold,
                            {{"A/B",language_text(model,3909,"Back")},{"X",tr_text(model,"Page","Стр.")}});
                    }
                }
                modern_done=true;
            } else if(tab==3 && model.skill_screen && model.guardians.slots[std::min<std::size_t>(4,model.selection)]) {
                // bn with named rows: a bar with both keys and their skills, the three skills, the chosen one described.
                const auto& g=*model.guardians.slots[std::min<std::size_t>(4,model.selection)];
                const auto sk=[&](int i){return model.guardian_skill_text && std::size_t(i)<model.guardian_skill_text->size()?
                    (*model.guardian_skill_text)[std::size_t(i)]:std::string();};
                static constexpr int skill_level[]{1,9,20,30};
                const int bar_h=22,assigned=model.skill_key_calm?g.calm:g.wild;
                plain([&]{
                    for(int k=0;k<2;++k) {
                        const bool on=(k==0)==model.skill_key_calm;const int w=lw/2,x=lx+k*w,skill=k==0?g.calm:g.wild;
                        bevel(x,m_top,w,bar_h,0x3f1f3f,0x9f7f3f,0x3f3f3f,on?0x3f1f3f:0x7f5f5f);
                        g_ui_hits.push_back({{cx+x*ms,cy+m_top*ms,w*ms,bar_h*ms},9,k,on,true});
                        const int cap=key_microcap(k==0?model.key_guardian_calm:model.key_guardian_wild,x+5,m_top+7);
                        if(skill>=0) {
                            image(500+6+g.id*4+skill,x+9+cap,m_top+3);
                            say(fit(sk(g.id*8+skill*2),w-cap-32),x+28+cap,m_top+7,on?W:T,w-cap-30);
                        }
                    }
                });
                std::vector<ModernRow> rows;
                for(int k=0;k<3;++k) {ModernRow row;row.icon=500+6+g.id*4+k;row.name=sk(g.id*8+k*2);row.mark=k==assigned;rows.push_back(row);}
                modern_list(rows,model.skill_selection,lx,m_top+bar_h+2,lw,m_body-bar_h-2,std::string());
                modern_panel(dx,m_top,dw,m_body);
                plain([&]{
                    say(sk(g.id*8+model.skill_selection*2),dx+6,m_top+6,W,dw-12);
                    const std::string level=language_text(model,3932,"LEVEL ");
                    label(level,dx+6,m_top+22);
                    digits(skill_level[model.skill_selection],dx+6+text_width(clean(level))+5,m_top+22,false);
                    say_paged(sk(g.id*8+model.skill_selection*2+1),dx+6,m_top+36,T,dw-12,m_body-40);
                });
                modern_done=true;
            } else {
                // Status and Save keep their original layout, widened, right of the tab column.
                cx=frame_x+(lx-2)*ms;cy=frame_y+(m_top-15+4)*ms;ex=std::max(0,MW-lx-6-155);split=100;
            }
        } else if(modern_shop) {
            // bp with named rows: categories on the left, goods with their price, the original details.
            modern_frame();
            std::vector<std::pair<int,std::string>> tabs;
            for(int i=0;i<6;++i) tabs.push_back({0x51000+i,st(i+1)});
            modern_tabs(tabs,model.category);
            const int lx=m_tabs_w+6,lw=std::min(210,(MW-lx-10)*11/20),dx=lx+lw+4,dw=MW-dx-6,list_top=m_top+14;
            plain([&]{label(model.selling?st(18):st(model.category+1),lx+1,m_top+1);});
            std::vector<ModernRow> rows;
            for(const auto& item:model.items) {
                ModernRow row;row.name=item.name;
                if(item.type>=0) row.icon=200+item.type;
                if(model.selling) row.count=item.count; else row.price=item.price;
                rows.push_back(row);
            }
            modern_list(rows,int(model.selection),lx,list_top,lw,m_body-14,std::string());
            modern_panel(dx,m_top,dw,m_body);
            if(model.confirming && model.selection<model.items.size() && model.items[model.selection].type>=0) {
                // ab in the detail panel: the item, the question, quantity arrows, total and balance.
                const auto& item=model.items[model.selection];
                const int line=int(font.height)+2;
                plain([&]{
                    image(200+item.type,dx+14,m_top+14,3);
                    say(item.name,dx+26,m_top+8,W,dw-30,line*2);
                    const bool choose=model.selling?item.count>1:item.stackable;
                    say(choose?st(model.selling?25:14):st(15),dx+6,m_top+36,W,dw-12,line*3);
                    if(choose) {
                        const int qy=m_top+80,qx=dx+dw/2;
                        image(0x50002,qx-22,qy);digits(model.quantity,qx+4,qy,true);image(0x50003,qx+12,qy);
                        g_ui_hits.push_back({{cx+(qx-32)*ms,cy+(qy-9)*ms,24*ms,24*ms},10,0,false,false});
                        g_ui_hits.push_back({{cx+(qx+2)*ms,cy+(qy-9)*ms,24*ms,24*ms},10,1,false,false});
                    }
                    R(dx+4,m_top+98,dw-8,1,0x3f1f3f);
                    say(model.selling?tr_text(model,"Receive","Получите"):tr_text(model,"Total","Итого"),dx+6,m_top+104,T,dw-12);
                    money_at(std::int64_t(item.price)*model.quantity,dx+dw-6,m_top+118);
                    say(st(13),dx+6,m_top+132,T,dw-12);
                    money_at(model.gold,dx+dw-6,m_top+146);
                });
            } else {
                modern_detail(dx,dw,33,14);
                if(model.selection<model.items.size() && model.items[model.selection].type>=0) {
                    const auto& item=model.items[model.selection];
                    item_detail(item);
                    // v: the better / worse marker of the selected goods, at the top right of the panel.
                    if(!model.selling && item.compare) plain([&]{image(item.compare>0?0x51007:0x51006,dx+dw-14,m_top+24,33);});
                }
                // sgui/shop 8/9 "#BUY" / "#SELL" as the platform button at the foot of the panel.
                draw_key_hints(m,latin,{frame_x+(dx+6)*ms,frame_y+(m_top+m_body-14)*ms,(dw-12)*ms,10*ms},W,
                    {{"Y",model.selling?bank_text(model,3914,"Buy"):bank_text(model,3912,"Sell")}});
            }
            modern_done=true;
        }
        if(service_ui || modern_done) {
        } else if(shop) {
            const auto& rows=model.items;
            if(model.confirming && model.selection<rows.size()) {
                // ab: Balance box, question box, quantity arrows, item icon, total price.
                const auto& item=rows[model.selection];
                bevel(3,20,149,29,0x1f1f3f,0x5f3f3f,0x1f1f3f,0x3f1f3f);
                bevel(3,51,149,67,0x1f1f3f,0x5f3f3f,0x1f1f3f,0x3f1f3f);
                say(st(13),26,27,T,80);money_at(model.gold,120,31);
                const bool choose=model.selling?item.count>1:item.stackable;
                say(choose?st(model.selling?25:14):st(15),24,58,W,125);
                if(choose) {
                    image(0x50002,50,85);digits(model.quantity,86,85,true);image(0x50003,95,85);
                    g_ui_hits.push_back({{cx+wx(40)*ms,cy+76*ms,24*ms,24*ms},10,0,false,false});
                    g_ui_hits.push_back({{cx+wx(88)*ms,cy+76*ms,24*ms,24*ms},10,1,false,false});
                }
                image(200+item.type,63,77);
                money_at(std::int64_t(item.price)*model.quantity,95,105);
            } else {
                // v / bb: tab title (or "Select item to sell"), slots, better/worse marker, details.
                label(model.selling?st(18):st(model.category+1),3,15);
                list(int(rows.size()),int(model.selection),true);
                const std::size_t first=(model.selection/5)*5;
                for(std::size_t i=first;i<first+5 && i<rows.size();++i) {
                    if(rows[i].type<0) continue;
                    image(200+rows[i].type,bx+13,by+18+23*int(i%5)+1,3);
                    if(model.selling && rows[i].count>1) digits(rows[i].count,bx+24,by+18+23*int(i%5)+2,true);
                }
                if(model.selection<rows.size()) {
                    const auto& item=rows[model.selection];
                    if(!model.selling && item.compare)
                        image(item.compare>0?0x51007:0x51006,bx+20,by+18+int(model.selection%5)*23,33);
                    if(item.type>=0) item_detail(item);
                }
                // sgui/shop 8/9 read "#BUY" / "#SELL" (phone key); show the platform button instead.
                bevel(155-54,170-22,52,14,0x1f1f3f,0x3f5f5f,0x1f1f3f,0x1f3f3f);
                draw_key_hints(m,latin,{cx+(155-51+ex)*ms,cy+(170-19)*ms,50*ms,10*ms},W,
                    {{"Y",model.selling?bank_text(model,3914,"Buy"):bank_text(model,3912,"Sell")}});
            }
        } else if(tab==0) {
            if(model.attribute_editing && model.bottom_pass) {
                // 3DS bottom screen: the same editor sized for fingers (rows 26px, 22px -/+ buttons).
                const SDL_Color dark=C(0x3f1f3f),row_fill=C(0x5f3f3f),pick=C(0x7f4f4f);
                const SDL_Rect panel{44,54,232,150}; // below the tab strip
                m.rect(panel,C(0x1f0f1f));m.rect({panel.x+1,panel.y+1,panel.w-2,panel.h-2},row_fill);
                const SDL_Rect head{panel.x+6,panel.y+6,panel.w-12,24};
                m.rect(head,dark);
                const std::string left=clean(gm(36))+" "+clean(gm(37));
                m.text(left,{head.x+6,head.y+7,head.w-50,int(font.height)+1},T);
                const std::string remaining=std::to_string(attribute_points_remaining(model.progress,model.pending_attributes));
                m.text(remaining,{head.x+head.w-8-text_width(remaining),head.y+7,40,int(font.height)+1},W);
                g_ui_hits.push_back({head,5,0,false,false});
                for(int i=0;i<4;++i) {
                    const int y=panel.y+36+i*28;const bool selected=model.attribute_selection==i;
                    if(selected) m.rect({panel.x+6,y,panel.w-12,26},pick);
                    m.text(clean(hero_label(9+i)),{panel.x+12,y+8,110,int(font.height)+1},selected?W:T);
                    const auto button=[&](int x,bool plus,int kind) {
                        const SDL_Rect b{x,y+2,22,22};
                        m.rect(b,C(0x1f0f1f));m.rect({b.x+1,b.y+1,b.w-2,b.h-2},C(0xdfbf7f));m.rect({b.x+2,b.y+2,b.w-4,b.h-4},dark);
                        m.rect({b.x+6,b.y+10,10,2},W);
                        if(plus) m.rect({b.x+10,b.y+6,2,10},W);
                        g_ui_hits.push_back({{b.x-4,y,b.w+8,26},kind,i,selected,false});
                    };
                    button(panel.x+130,false,3);
                    const std::string value=std::to_string(model.progress.attributes[i]+model.progress.gear_bonus[i]+model.pending_attributes[i]);
                    m.text(value,{panel.x+176-text_width(value)/2,y+8,30,int(font.height)+1},model.pending_attributes[i]>0?C(0xffdf7f):W);
                    button(panel.x+198,true,4);
                    g_ui_hits.push_back({{panel.x+6,y,120,26},2,i,selected,false});
                }
            } else if(model.attribute_editing) {
                // bi replaces q: origin (36,37); Available/Point box, four rows with </> arrows.
                const int ox=36,oy=37;
                rounded(ox,oy,101,26,0x3f1f3f);
                label(gm(36),ox+3,oy+3);label(gm(37),ox+3,oy+14);
                digits(attribute_points_remaining(model.progress,model.pending_attributes),ox+65,oy+14,true);
                R(ox,oy+30,101,62,0x5f3f3f);
                for(int i=0;i<4;++i) {
                    const int y=oy+35+i*15;const bool selected=model.attribute_selection==i;
                    if(selected) image(0x50003,ox+2,y);
                    say(hero_label(9+i),ox+10,y,selected?W:T,55);
                    image(0x50002,ox+70,y);
                    digits(model.progress.attributes[i]+model.progress.gear_bonus[i]+model.pending_attributes[i],ox+90,y,true); // bi: base + gear + pending
                    image(0x50003,ox+92,y);
                }
            } else {
            // q.a(Graphics,int,int)
            label(gm(2),bx+5,by);money();
            list(4,model.status_page,false);
            for(int i=0;i<3;++i) digits(i+1,bx+12,by+16+23*i,false);
            image(0x50008,bx+10,by+83);
            if(model.progress.attribute_points>0) image(0x50006,bx+3,by+87,36);
            status_page_detail();
            }
        } else if(tab==1 || tab==2) {
            // ay / bz: item slots at (13, 18+23i), detail cb.a(g, 33, 14, item).
            const auto& rows=tab==1?model.items:model.equipped;
            label(tab==1?com(2):gm(20),bx+5,by);money();
            list(tab==1?int(rows.size()):5,int(model.selection),tab==1);
            const std::size_t first=(model.selection/5)*5;
            for(std::size_t i=first;i<first+5 && i<rows.size();++i) {
                const int row=int(i%5);
                if(rows[i].type>=0) {
                    image(200+rows[i].type,bx+13,by+18+23*row+1,3);
                    if(tab==1 && rows[i].count>1) digits(rows[i].count,bx+13+11,by+18+23*row+2,true);
                } else if(tab==2) image(307+row,bx+13,by+19+23*row,3);
            }
            const int x=bx+33;const int y=by+14;
            if(model.selection<rows.size() && rows[model.selection].type>=0) item_detail(rows[model.selection]);
            else if(tab==1) say(gm(15),x,y,W,115);
            else if(model.selection==1 && model.character_class!=8) say(gm(49),bx+30,y,W,115,30);
            else say(gm(21),x,y,W,115);
        } else if(tab==3 && model.skill_screen && model.guardians.slots[std::min<std::size_t>(4,model.selection)]) {
            // bn replaces bm: key 1 / key 3 tabs, three skills with icon, name, LEVEL, description.
            const auto& g=*model.guardians.slots[std::min<std::size_t>(4,model.selection)];
            const auto sk=[&](int i){return model.guardian_skill_text && std::size_t(i)<model.guardian_skill_text->size()?
                (*model.guardian_skill_text)[std::size_t(i)]:std::string();};
            static constexpr int skill_level[]{1,9,20,30};
            const int x=2,y=10;
            // The key tabs keep their original geometry; the Wide split would tear the second tab.
            plain([&]{
                bevel(x,y,132,22,0x1f1f3f,0x5f3f3f,0x1f1f3f,0x3f1f3f);
                const int t=x+(model.skill_key_calm?0:66);
                R(t+2,y+2,62,19,0x5f3f3f);R(t+64,y+1,1,19,0x3f3f3f);
                R(t+65,y+1,1,17,0x1f1f3f);R(t,y+1,1,18,0x1f1f3f);R(t+1,y,64,1,0x1f1f3f);
                R(t+1,y+2,1,18,0x9f7f3f);R(t+1,y+1,63,1,0x9f7f3f);
            });
            bevel(x,y+19,149,140,0x1f1f3f,0x9f7f3f,0x3f3f3f,0x5f3f3f);
            // "Key 1 / Key 3" named the phone keypad; show the platform's guardian buttons instead.
            const auto key=[&](int lx,int ly,const std::string& name,int skill) {
                plain([&]{
                    const std::string text=clean(gm(38));label(text,lx,ly);
                    const int end=lx+text_width(text)+2;
                    const int cap=key_microcap(name,end+3,ly-1);
                    if(skill>=0) image(500+6+g.id*4+skill,end+5+cap,ly-2);
                });
            };
            key(x+6,y+5+(model.skill_key_calm?0:2),model.key_guardian_calm,g.calm);
            key(x+71,y+5+(model.skill_key_calm?2:0),model.key_guardian_wild,g.wild);
            plain([&]{image(0x50002,x,y+7);image(0x50003,x+130,y+7);});
            rounded(x+28,y+26,114,127,0x3f1f3f);
            rounded(x+3,y+31+model.skill_selection*20,26,19,0x3f1f3f);
            for(int k=0;k<3;++k) image(500+6+g.id*4+k,x+5,y+48+k*20,36);
            for(int k=0;k<2;++k) g_ui_hits.push_back({{cx+(x+k*66)*ms,cy+y*ms,64*ms,20*ms},9,k,(k==0)==model.skill_key_calm,false});
            for(int k=0;k<3;++k) g_ui_hits.push_back({{cx+(x+1)*ms,cy+(y+30+k*20)*ms,28*ms,20*ms},2,k,k==model.skill_selection,false});
            say(sk(g.id*8+model.skill_selection*2),x+34,y+29,W,110);
            const std::string level=language_text(model,3932,"LEVEL ");
            label(level,x+34,y+44);
            digits(skill_level[model.skill_selection],x+34+text_width(clean(level))+2+3,y+44,false);
            say(sk(g.id*8+model.skill_selection*2+1),x+34,y+53,T,100,95);
        } else if(tab==3) {
            // bm: grdico at (15, 19+23i); Equip label, LV/EXP/NEXT block.
            label(gm(30),bx+5,by);
            list(5,int(model.selection),false);
            for(std::size_t i=0;i<model.guardians.slots.size() && i<5;++i)
                if(const auto& g=model.guardians.slots[i]) image(500+g->id,bx+15,by+19+int(i)*23,3);
            const auto& selected=model.guardians.slots[std::min<std::size_t>(4,model.selection)];
            const auto gtext=[&](int i) -> std::string {
                return i>=0 && std::size_t(i)<model.guardian_text.size()?model.guardian_text[i]:"?";
            };
            if(!selected) say(gm(31),bx+34,by+18,T);
            else {
                const auto& g=*selected;
                plain([&]{
                    label(gm(32),bx+89+ex,by+22);
                    if(model.selection==model.guardians.equipped) image(0x50006,bx+100+ex,by+26,36);
                });
                say(gtext(g.id),bx+34,by+18,W,55);
                say(gtext(g.id+6),bx+34,by+35,T,110);
                image(0x5000b,bx+34,by+53);digits(g.level,bx+50,by+53,false);
                image(0x50009,bx+34,by+67);digits(g.experience,bx+102,by+67,true);
                const int next=std::max(1,g.next_experience());
                R(bx+34,by+76,72,3,0x3f1f3f);R(bx+35,by+77,70*std::min(g.experience,next)/next,1,0xffffff);
                image(0x5000c,bx+38,by+81);digits(next,bx+102,by+81,true);
            }
        } else if(tab==4 && model.recipes_view) {
            // Port recipe journal in the s layout: result icons in the slots, the formula beside them.
            label(tr_text(model,"Recipes","Рецепты"),bx+5,by);
            if(model.recipes.empty()) {
                bevel(bx+4,by+10,143,137,0x3f1f3f,0x9f7f3f,0x3f3f3f,0x5f3f3f);
                say(tr_text(model,"No recipes yet.","Рецептов пока нет."),bx+10,by+15,W,96,40);
            } else {
                list(int(model.recipes.size()),int(model.selection),true);
                const std::size_t first=(model.selection/5)*5;
                for(std::size_t i=first;i<first+5 && i<model.recipes.size();++i)
                    if(model.recipes[i].icon_type>=0) image(200+model.recipes[i].icon_type,bx+13,by+18+23*int(i%5)+1,3);
                const auto& recipe=model.recipes[std::min(model.selection,model.recipes.size()-1)];
                int y=by+14;
                say(recipe.result,bx+33,y,W,105,30);y+=line_count(clean(recipe.result),105+ex)*15;
                for(const auto& part:recipe.parts) {
                    say("+ "+part.first,bx+33,y,part.second>0?W:T,90);
                    if(part.second>0) digits(part.second,bx+140,y+1,true);
                    y+=14;
                }
            }
        } else if(tab==4) {
            // s: quest icon icoitm[18], name, Main/Sub, In progress, location and giver lines.
            label(gm(39),bx+5,by);
            if(model.quests.empty()) {
                bevel(bx+4,by+10,143,137,0x3f1f3f,0x9f7f3f,0x3f3f3f,0x5f3f3f);
                say(gm(58),bx+10,by+15,W,96,40);
            } else {
                list(int(model.quests.size()),int(model.selection),true);
                const std::size_t first=(model.selection/5)*5;
                for(std::size_t i=first;i<first+5 && i<model.quests.size();++i) image(218,bx+13,by+18+23*int(i%5),3);
                const auto& quest=model.quests[std::min(model.selection,model.quests.size()-1)];
                int y=by+14;
                say(quest.name,bx+33,y,W,105,45);y+=line_count(quest.name,105+ex)*15;
                say(gm(quest.main_quest?55:56),bx+33,y,T,105);y+=15;
                say(gm(57),bx+33,y,T,105);y+=15;
                if(!quest.location.empty()) {say(quest.location,bx+33,y,W,105);y+=15;}
                if(!quest.giver.empty()) say(quest.giver,bx+33,y,W,105);
                if(model.quest_reading) {
                    m.rect({0,0,output_w,output_h},{0,0,0,150});
                    SDL_Rect modal{(output_w-(220+ex)*ms)/2,(output_h-154*ms)/2,(220+ex)*ms,154*ms};
                    m.rect(modal,ink);m.outline(modal,muted,ms);
                    m.text(quest.name,{modal.x+6*ms,modal.y+6*ms,modal.w-12*ms,24*ms},gold);
                    m.paragraph(quest.description+"\n\n"+clean(gm(54))+"\n"+quest.reward,
                        {modal.x+6*ms,modal.y+34*ms,modal.w-12*ms,100*ms},model.detail_page);
                    draw_key_hints(m,latin,{modal.x+6*ms,modal.y+140*ms,modal.w-12*ms,10*ms},gold,
                        {{"A/B",language_text(model,3909,"Back")},{"X",tr_text(model,"Page","Стр.")}});
                }
            }
        } else {
            // d: four 108px buttons centred on the screen, 22px apart (Save/Help/Options/Quit).
            label(gm(41),bx+5,by);
            for(int i=0;i<4;++i) {
                const bool selected=model.selection==std::size_t(i);
                const int x=77-54+ex/2,y=by+15+22*i;
                g_ui_hits.push_back({{cx+x*ms,cy+y*ms,108*ms,19*ms},2,i,selected,false});
                plain([&]{
                    if(selected) bevel(x,y,108,19,0x3f1f3f,0x9f7f3f,0x3f3f3f,0x5f1f3f);
                    else bevel(x,y,108,19,0x5f3f3f,0xdfbf7f,0x7f5f7f,0x9f7f7f);
                    const std::string text=clean(gm(42+i));
                    say(text,x+54-text_width(text)/2,y+5,W,108);
                });
            }
        }
        // 3DS bottom screen: the key hints become a row of finger-sized buttons under the box.
        const auto touch_keys=[&](const std::vector<std::pair<const char*,std::string>>& keys) {
            const int n=int(keys.size());if(n==0) return;
            const int gap=4,w=(output_w-12-(n-1)*gap)/n,h=22,y=output_h-h-2;
            for(int i=0;i<n;++i) {
                const SDL_Rect b{6+i*(w+gap),y,w,h};
                bottom_ui::frame(m,b);
                const std::string& label=keys[std::size_t(i)].second;
                const int cap=key_cap(latin,keys[std::size_t(i)].first,0,0,10,gold,false);
                const int total=cap+4+text_width(label);
                const int x=b.x+std::max(5,(b.w-total)/2);
                key_cap(latin,keys[std::size_t(i)].first,x,b.y+6,10,gold,true);
                m.text(label,{x+cap+4,b.y+7,b.x+b.w-(x+cap+4)-4,int(font.height)+1},W);
                const std::string key=keys[std::size_t(i)].first;
                const int code=key=="A"?0:key=="B"?1:key=="X"?2:key=="Y"?3:-1;
                if(code>=0) g_ui_hits.push_back({b,7,code,false,false});
            }
        };
        // Modern: hints sit under the frame; dialogs return to the centred original box.
        if(modern) {cx=frame_x+20*ms;cy=frame_y+(MH-176)*ms;ex=wide_ex;split=100;}
        const bool dialog_open=model.confirming || !model.notice.empty() || (tab==3 && model.guardian_choice>=0) ||
            (tab==1 && model.item_action);
        if(!dialog_open && !shop && !service_ui) {
            // Port control hints: words from the JAR where it has them, otherwise English/Russian.
            const auto tr=[&](const char* en,const char* ru){return model.localized?std::string(ru):std::string(en);};
            const bool keys_mode=model.skill_screen && tab==3;
            if(!model.bottom_pass) m.text(keys_mode?tr("Left/Right: key  Up/Down: skill","Влево/вправо - ключ, вверх/вниз - навык"):
                             !modern?tr("L/R, Left/Right: tabs  Up/Down: select","L/R, влево/вправо - вкладки, вверх/вниз - выбор"):
                             model.tab_focus?tr("Up/Down: tab  Right/A: list","вверх/вниз - вкладка, вправо/A - список"):
                             tr("Left: tabs  Up/Down: select","влево - вкладки, вверх/вниз - выбор"),
                   {std::max(2*ms,cx-20*ms),cy+178*ms,280*ms,10*ms},muted); // stays on a 640-wide screen
            const std::string back=language_text(model,3909,"Back");
            std::vector<std::pair<const char*,std::string>> keys;
            const std::pair<const char*,std::string> more{"X",tr("More","Далее")};
            if(tab==0 && model.attribute_editing) keys={{"A",tr("Save","Сохранить")},{"B",back}}; // bi: A confirms the points
            else if(tab==0 && model.status_page==3 && model.progress.attribute_points>0)
                keys={{"A",tr("Distribute","Распределить")},{"B",back}}; // Capacity: A opens the point editor
            else if(tab==4 && model.quest_reading) keys={{"X",tr("Page","Стр.")},{"B",back}}; // reading: pages and back
            else if(tab==1 && text_pages) keys={{"A",clean(gm(13))+"/"+clean(gm(11))},more,{"B",back}};
            else if(tab==1) keys={{"A",clean(gm(13))+"/"+clean(gm(11))},{"B",back}};
            else if(tab==2) keys={{"A",tr("Unequip","Снять")},{"B",back}};
            else if(tab==3) keys={{"A",language_text(model,3913,"Select")},{"B",back}};
            else if(tab==4 && model.recipes_view) keys={{"Y",tr("Quests","Задания")},{"B",back}};
            else if(tab==4) keys={{"A",tr("Read","Читать")},{"X",tr("Page","Стр.")},{"Y",tr("Recipes","Рецепты")},{"B",back}};
            else if(tab==5) keys={{"A",language_text(model,3913,"Select")},{"B",back}};
            else if(text_pages) keys={more,{"B",back}};
            else keys={{"B",back}};
            if(model.bottom_pass) touch_keys(keys);
            int kx=cx;
            if(!model.bottom_pass) for(const auto& key:keys) {
                kx+=key_cap(latin,key.first,kx,cy+189*ms,10*ms,gold,true)+3*ms;
                m.text(key.second,{kx,cy+189*ms,100*ms,10*ms},gold);
                kx+=(text_width(key.second)+10)*ms;
            }
        }
        cx=output_w/2-(77+wide_ex/2)*ms;cy=output_h/2-85*ms;ex=wide_ex;split=100; // Modern drew elsewhere
        // af: 80% of the screen wide, centred; cb.a dark bevel + 0x3f1f3f fill, white text at +5.
        // Types: 1 message (Ok), 2 question (Yes/No, text height + 8), 3 list (Ok/Back, ce.d cursor).
        const auto dialog=[&](int type,const std::vector<std::string>& entries,int selection) {
            const int w=240*80/100,text_w=w-10;
            const auto height_of=[&](const std::string& text){return line_count(clean(text),text_w)*int(font.height);};
            std::string joined;
            for(std::size_t i=0;i<entries.size();++i) if(!entries[i].empty()) {
                if(!joined.empty()) joined+="\n";
                joined+=entries[i];
            }
            int h=type==2?8+height_of(joined):22+height_of(joined);
            if(type==3 || type==8) {h=12;for(const auto& e:entries) h+=3+height_of(e);}
            // Centred on the box as drawn: the phone box (77,85) widened by ex, or the Modern frame.
            // Drawn unmapped: past the split column the right half moved by ex, stretching the box
            // off the left edge on narrow screens (3DS bottom screen).
            const int saved_split=split;split=1<<20;
            const int x=(modern?MW/2:77+ex/2)-w/2,y=(modern?MH/2:85)-h/2;
            bevel(x,y,w,h,0x1f1f3f,0x5f3f3f,0x1f1f3f,0x3f1f3f);
            if(type==3 || type==8) {
                // Type 8 heads the list with a question line (aa / ax "What do you need?").
                int ly=y+7;
                for(std::size_t i=0;i<entries.size();++i) {
                    const int row=type==8?int(i)-1:int(i);
                    if(row>=0 && row==selection) image(0x50003,x+5,ly);
                    if(row>=0) g_ui_hits.push_back({{cx+(x+2)*ms,cy+(ly-2)*ms,(w-4)*ms,(height_of(entries[i])+3)*ms},6,row,row==selection,false});
                    say(entries[i],row<0?x+5:x+12,ly,W,text_w,height_of(entries[i]));
                    ly+=3+height_of(entries[i]);
                }
            } else say(joined,x+5,y+5,W,text_w,height_of(joined));
            split=saved_split;
        };
        const auto softkeys=[&](const std::string& left,const std::string& right) { // bh.a(g, left, right)
            if(model.bottom_pass) {
                std::vector<std::pair<const char*,std::string>> keys;
                if(!left.empty()) keys.push_back({"A",left});
                if(!right.empty()) keys.push_back({"B",right});
                touch_keys(keys);return;
            }
            if(!left.empty()) {const int kw=std::max(key_cap(latin,"A",2*ms,output_h-12*ms,10*ms,gold,true)+3*ms,14*ms);
                m.text(left,{2*ms+kw,output_h-12*ms,90*ms,10*ms},gold);}
            if(!right.empty()) {const int kw=std::max(key_cap(latin,"B",output_w-80*ms,output_h-12*ms,10*ms,gold,true)+3*ms,14*ms);
                m.text(right,{output_w-80*ms+kw,output_h-12*ms,64*ms,10*ms},gold);}
        };
        const auto bank=[&](int i,const char* fallback){return language_text(model,i,fallback);};
        if(service_ui) {
            const auto& v=*model.service;
            const auto t=[&](int i){return v.text && std::size_t(i)<v.text->size()?(*v.text)[std::size_t(i)]:std::string();};
            const auto panel=[&](int x,int y,int w,int h){lines(x,y,w,h,0xffdfbf,0xbf9f7f);R(x+1,y+1,w-2,h-2,0xdfbf7f);}; // cb.c
            const auto field=[&](int x,int y,int w,int h,bool selected) { // cb.a(g,x,y,w,h,boolean)
                if(selected) bevel(x,y,w,h,0x3f1f3f,0x9f7f3f,0x3f3f3f,0x5f1f3f);
                else bevel(x,y,w,h,0x5f403f,0xdfbf7f,0x7f5f7f,0x9f7f7f);
            };
            int field_index=0; // item boxes, then the button: the order of v.cursor
            const auto button=[&](int y,const std::string& text,bool selected) { // cb.a(g,x,y,w,char[],boolean)
                const int w=124,x=(155-w)/2; // bh.a(155,80)
                field(x,y,w,19,selected);
                g_ui_hits.push_back({{cx+wx(x)*ms,cy+y*ms,(wx(x+w)-wx(x))*ms,19*ms},2,field_index++,selected,false});
                const int centre=(wx(x)+wx(x+w))/2;
                plain([&]{say(text,centre-text_width(clean(text))/2,y+5,W,w);});
            };
            const auto item_box=[&](int x,int y,const VisualItem* item,int number,const std::string& text,bool selected) {
                // cb.a(g,x,y,ad,byte,char[],boolean): icon well, "Item n" label, caption, name field.
                rounded(x,y+1,28,31,0xbf9f7f);
                g_ui_hits.push_back({{cx+wx(x)*ms,cy+y*ms,(wx(x+147)-wx(x))*ms,34*ms},2,field_index++,selected,false});
                const std::string name=clean(com(2));
                label(name,x+2,y+1);digits(number,x+4+text_width(name),y+1,false);
                say(text,x+90,y+2,W,60);
                field(x+30,y+14,117,19,selected);
                if(item) {
                    image(200+item->type,x+14,y+19,3);
                    say(item->power>=0 && !item->identified?item->type_name:item->name,x+34,y+20,W,110);
                }
            };
            if(v.page>=0) {
                R(0,0,155,170,0x3f1f3f);panel(2,4,151,162);
                if(v.blacksmith && v.page==0) { // at
                    label(t(13),3,-2);
                    rounded(3,7,149,17,0x9f7f7f);say(t(14),6,11,W,145);
                    item_box(4,30,v.has_item?&v.item:nullptr,1,t(15),v.cursor==0);
                    money_at(model.gold,147,65);
                    rounded(4,73,147,38,0x9f7f7f);say(t(16),8,80,W,100);
                    if(v.has_item) {money_at(v.cost,147,80);say(clean(t(17))+" "+std::to_string(v.need),8,93,W,140);}
                    button(138,t(18),v.cursor==1);
                } else if(v.blacksmith) { // ch
                    label(t(13),3,-2);
                    rounded(3,7,149,17,0x9f7f7f);say(t(23),6,10,W,145);
                    item_box(4,30,v.has_item?&v.item:nullptr,1,t(15),v.cursor==0);
                    money_at(model.gold,145,75);
                    rounded(4,83,147,20,0x9f7f7f);say(t(24),8,88,W,100);money_at(100,147,89);
                    button(138,t(25),v.cursor==1);
                } else if(v.page==0) { // ap
                    label(t(14),3,-2);
                    item_box(4,9,v.has_item?&v.item:nullptr,1,t(15),v.cursor==0);
                    item_box(4,45,v.has_orb?&v.orb:nullptr,2,t(16),v.cursor==1);
                    rounded(4,81,147,31,0xbf9f7f);
                    if(v.has_item && v.has_orb) {
                        int x=6;
                        for(const auto& part:{v.option_name,t(17),t(18)}) {say(part,x,85,W,std::max(10,150-x));x+=2+text_width(clean(part));}
                        money_at(500,145,86);money_at(model.gold,145,101);
                    }
                    button(138,t(19),v.cursor==2);
                } else { // k
                    label(t(14),3,-2);
                    for(std::size_t i=0;i<3;++i) item_box(4,9+36*int(i),v.has_mix[i]?&v.mix[i]:nullptr,int(i)+1,t(33),v.cursor==int(i));
                    button(138,t(25),v.cursor==3);
                }
            }
            if(v.picking && model.modern_ui) {
                // Modern m: the title over a named list, the original details in a panel beside it.
                modern_frame();
                const int lx=6,lw=std::min(220,(MW-16)*11/20),dx=lx+lw+4,dw=MW-dx-6,list_top=m_top+14;
                plain([&]{label(t(v.pick_title),lx+1,m_top+1);});
                std::vector<ModernRow> rows;
                for(const auto& item:v.picks) {
                    ModernRow row;row.name=item.power>=0 && !item.identified?item.type_name:item.name;row.count=item.count;
                    if(item.type>=0) row.icon=200+item.type;
                    rows.push_back(row);
                }
                modern_list(rows,v.pick_sel,lx,list_top,lw,m_body-14,std::string());
                modern_panel(dx,m_top,dw,m_body);
                modern_detail(dx,dw,33,14);
                if(std::size_t(v.pick_sel)<v.picks.size()) item_detail(v.picks[std::size_t(v.pick_sel)]);
                cx=output_w/2-(77+wide_ex/2)*ms;cy=output_h/2-85*ms;ex=wide_ex;split=100;
            } else if(v.picking) {
                // m: panel, title bar (3px lower for a single entry), slot list, selected item's details.
                const bool multi=v.picks.size()>1;
                panel(2,-3,151,170);
                rounded(5,-2+(multi?0:3),145,14,0x9f7f7f);say(t(v.pick_title),8,1+(multi?0:3),W,140);
                bx=2;by=11;
                list(int(v.picks.size()),v.pick_sel,multi);
                const std::size_t first=std::size_t(v.pick_sel/5)*5;
                for(std::size_t i=first;i<first+5 && i<v.picks.size();++i) {
                    image(200+v.picks[i].type,bx+13,by+18+23*int(i%5)+1,3);
                    if(v.picks[i].count>1) digits(v.picks[i].count,bx+24,by+18+23*int(i%5)+2,true);
                }
                if(std::size_t(v.pick_sel)<v.picks.size()) item_detail(v.picks[std::size_t(v.pick_sel)]);
            }
            if(v.mix_confirm) { // bo: chosen materials, gold, "Mix cost" 500
                panel(10,25,135,120);
                rounded(13,28,129,17,0x9f7f7f);say(t(25),16,29,W,125);
                rounded(13,50,129,60,0x9f7f7f);
                int row=0;
                for(std::size_t i=0;i<3;++i) if(v.has_mix[i]) say(v.mix[i].name,16,52+18*row++,W,125);
                money_at(model.gold,140,115);
                rounded(13,123,129,15,0x9f7f7f);say(t(26),16,124,W,100);money_at(500,140,130);
            }
            if(v.busy) {bevel(22,74,110,22,0x1f1f3f,0x5f3f3f,0x1f1f3f,0x3f1f3f);say(t(28),27,79,W,100);}
            if(!v.message.empty()) {
                dialog(v.question?2:1,{v.message},0);
                if(v.question) softkeys(bank(3915,"Yes"),bank(3916,"No"));else softkeys(bank(3908,"Ok"),"");
            } else if(v.page<0) {
                dialog(8,{t(0),t(1),t(2)},v.menu);softkeys(bank(3913,"Select"),bank(3909,"Back"));
            } else if(!v.busy) softkeys(v.mix_confirm?bank(3908,"Ok"):bank(3913,"Select"),bank(3909,"Back"));
            return;
        }
        if(shop) {
            // af over the shop: question (Yes/No), result message (Ok); otherwise bh softkeys.
            if(model.shop_asking) {dialog(2,{model.shop_question},0);softkeys(bank(3915,"Yes"),bank(3916,"No"));}
            else if(!model.notice.empty()) {dialog(1,{model.notice},0);softkeys(bank(3908,"Ok"),"");}
            else if(model.confirming) softkeys(model.selling?bank(3912,"Sell"):bank(3914,"Buy"),bank(3909,"Back"));
            else if(model.bottom_pass)
                touch_keys(text_pages?
                    std::vector<std::pair<const char*,std::string>>{{"A",bank(3913,"Select")},{"X",tr_text(model,"More","Далее")},
                        {"Y",model.selling?bank(3914,"Buy"):bank(3912,"Sell")},{"B",bank(3909,"Back")}}:
                    std::vector<std::pair<const char*,std::string>>{{"A",bank(3913,"Select")},
                        {"Y",model.selling?bank(3914,"Buy"):bank(3912,"Sell")},{"B",bank(3909,"Back")}});
            else {
                softkeys(bank(3913,"Select"),bank(3909,"Back"));
                if(text_pages) draw_key_hints(m,latin,{std::max(2*ms,cx-20*ms),cy+172*ms,280*ms,10*ms},gold,
                    {{"X",tr_text(model,"More","Далее")}});
            }
            return;
        }
        if(model.confirming) {
            dialog(2,{model.modal_title,model.modal_text},0);
            softkeys(bank(3915,"Yes"),bank(3916,"No"));
        } else if(tab==3 && model.guardian_choice>=0) {
            dialog(3,{gm(22),gm(23)},model.guardian_choice);
            softkeys(bank(3908,"Ok"),bank(3909,"Back"));
        } else if(tab==1 && (model.item_action==4 || model.item_action==5)) {
            dialog(3,{gm(model.item_action==4?11:13),gm(10)},model.item_choice);
            softkeys(bank(3908,"Ok"),bank(3909,"Back"));
        } else if(tab==1 && model.item_action==6) {
            dialog(2,{model.item_prompt},0);
            softkeys(bank(3908,"Ok"),bank(3909,"Back"));
        } else if(!model.notice.empty()) {
            dialog(1,{model.notice},0);
            softkeys(bank(3908,"Ok"),"");
        }
        return;
    }
    p.rect({0,0,output_w,output_h},{2,5,10,210});
    SDL_Rect outer{12*s,12*s,output_w-24*s,output_h-24*s};
    if(model.screen==VisualScreen::inventory) {
        const int ow=std::min(output_w-24*s,312*s),oh=std::min(output_h-24*s,330*s);
        outer={(output_w-ow)/2,(output_h-oh)/2,ow,oh};
    }
    p.frame(outer);
    p.text(model.title,{outer.x+14*s,outer.y+10*s,outer.w-220*s,24*s},gold);
    p.text(tr_text(model,"GOLD","ЗОЛОТО")+"  "+std::to_string(model.gold),{outer.x+outer.w-195*s,outer.y+10*s,180*s,24*s},gold);
    const int x=outer.x+12*s,y=outer.y+44*s,w=outer.w-24*s,h=outer.h-82*s;
    if (model.screen==VisualScreen::inventory) {
        // Original Item layout: five slots on the left, selected item details on the right.
        const int mw=std::min(w,288*s), mh=std::min(h,270*s);
        SDL_Rect menu{x+(w-mw)/2,y+(h-mh)/2,mw,mh};
        p.rect(menu,muted);p.outline(menu,ink,2*s);
        p.rect({menu.x+4*s,menu.y+4*s,menu.w-8*s,16*s},ink);
        p.text("Item",{menu.x+8*s,menu.y+6*s,menu.w-16*s,12*s},gold);
        const int list_x=menu.x+8*s, list_y=menu.y+24*s;
        const std::size_t first=(model.selection/5)*5;
        for(int row=0;row<5;++row) {
            const auto index=first+std::size_t(row);
            SDL_Rect slot{list_x,list_y+row*36*s,38*s,28*s};
            p.icon(index<model.items.size()?model.items[index].type:-1,slot);
            if(index==model.selection && index<model.items.size()) p.outline(slot,ink,2*s);
        }
        SDL_Rect detail{list_x+38*s,list_y,menu.w-54*s,menu.h-38*s};
        p.rect(detail,panel);p.outline(detail,ink,2*s);
        if(model.selection<model.items.size()) {
            const auto& item=model.items[model.selection];
            const int dx=detail.x+8*s,dw=detail.w-16*s;
            p.text(item.name,{dx,detail.y+7*s,dw,30*s},gold);
            p.text(category_name(model,item.type)+"  "+tr_text(model,"x","шт. ")+std::to_string(item.count),
                   {dx,detail.y+40*s,dw,24*s},gold);
            p.text(item.stats,{dx,detail.y+68*s,dw,30*s},white);
            p.paragraph(item.description,{dx,detail.y+104*s,dw,detail.h-112*s},model.detail_page);
        } else p.text("Your bag is empty.",{detail.x+8*s,detail.y+10*s,detail.w-16*s,30*s},gold);
        p.text(std::to_string(model.items.empty()?0:model.selection+1)+"/"+std::to_string(model.items.size()),
               {menu.x+8*s,menu.y+menu.h-14*s,menu.w-16*s,12*s},ink);
    } else if (model.screen==VisualScreen::shop) {
        const std::string tabs[]={tr_text(model,"Supplies","Припасы"),tr_text(model,"Weapons","Оружие"),
            tr_text(model,"Shields","Щиты"),tr_text(model,"Armor","Доспехи"),tr_text(model,"Headgear","Шлемы"),
            tr_text(model,"Accessories","Украшения")};
        const int types[]={7,0,3,5,6,4};
        if (!model.selling) {
            const int tw=w/6;
            for(int i=0;i<6;++i) {
                SDL_Rect tab{x+i*tw,y,tw-3*s,30*s};
                p.button(tab,"",i==model.category); p.icon(types[i],{tab.x+4*s,tab.y+4*s,22*s,22*s});
                p.text(tabs[i],{tab.x+30*s,tab.y+10*s,tab.w-32*s,15*s},i==model.category?gold:muted);
            }
        } else p.text(tr_text(model,"SELL FROM YOUR BAG","ПРОДАЖА ИЗ СУМКИ"),{x+8*s,y+8*s,w,20*s},gold);
        const int list_width=w/2-6*s;
        SDL_Rect list{x,y+40*s,list_width,h-40*s}; p.frame(list);
        const int row=32*s,visible=std::max(1,(list.h-30*s)/row);
        const auto first=(model.selection/std::size_t(visible))*std::size_t(visible);
        for (int n=0;n<visible && first+std::size_t(n)<model.items.size();++n) {
            const auto index=first+std::size_t(n); const auto& item=model.items[index];
            SDL_Rect r{list.x+8*s,list.y+8*s+n*row,list.w-16*s,row-3*s};
            if(index==model.selection) {p.rect(r,{66,55,36,255}); p.outline(r,gold,s);}
            p.icon(item.type,{r.x+3*s,r.y+3*s,23*s,23*s});
            p.line(item.name,{r.x+33*s,r.y+8*s,r.w-110*s,16*s},index==model.selection?gold:white);
            p.text(std::to_string(item.price)+" "+bank_text(model,3948,"G"),{r.x+r.w-70*s,r.y+8*s,66*s,16*s},gold);
        }
        if(!model.items.empty()) p.text(std::to_string(model.selection+1)+" / "+std::to_string(model.items.size()),
            {list.x+14*s,list.y+list.h-19*s,list.w-28*s,14*s},muted);
        if(model.items.empty()) p.text(model.selling?tr_text(model,"Nothing to sell.","Нечего продать."):tr_text(model,"No stock.","Нет товара."),{list.x+14*s,list.y+20*s,list.w-28*s,24*s},muted);
        detail_card(p,{x+list_width+12*s,y+40*s,w-list_width-12*s,h-40*s},model);
    } else if (model.screen==VisualScreen::pause) {
        const int bw=200*s,bx=x+24*s,by=y+20*s;
        const char* labels[]={"RESUME","INVENTORY","CONTROLS"};
        for(int i=0;i<3;++i) p.button({bx,by+i*52*s,bw,40*s},labels[i],model.selection==std::size_t(i));
        SDL_Rect portrait{x+w-230*s,y+12*s,200*s,h-24*s}; p.frame(portrait);
        p.text("HERO",{portrait.x+16*s,portrait.y+16*s,portrait.w-32*s,20*s},gold);
        if(hero_preview) hero_preview({portrait.x+24*s,portrait.y+48*s,portrait.w-48*s,portrait.h-72*s});
    } else if (model.screen==VisualScreen::controls) {
        const char* keys[]={"D-PAD","A / ENTER","B / ESC","X / Y","+ MENU"};
        const char* descriptions[]={"Move in the world. Select a bag slot or a menu option.",
            "Talk, turn the page, open an action or confirm.","Cancel an action or return to the previous screen.",
            "X pages descriptions. Y switches buy/sell in the shop.","Open or close the pause menu."};
        for(int i=0;i<5;++i) {
            SDL_Rect row{x+14*s,y+(8+46*i)*s,w-28*s,39*s};
            p.rect(row,raised);p.outline(row,{65,81,101,255},s);
            p.button({row.x+5*s,row.y+5*s,105*s,29*s},keys[i],i==0);
            p.text(descriptions[i],{row.x+123*s,row.y+8*s,row.w-135*s,26*s},white);
        }
    } else if (model.screen==VisualScreen::choice) {
        p.paragraph(model.text,{x+24*s,y+16*s,w-48*s,h-90*s},model.detail_page);
        p.button({x+w/2-164*s,y+h-65*s,150*s,40*s},"YES",model.choice_yes);
        p.button({x+w/2+14*s,y+h-65*s,150*s,40*s},"NO",!model.choice_yes);
    } else {
        p.paragraph(model.text,{x+24*s,y+20*s,w-48*s,h-40*s},model.detail_page);
    }
    Painter pl{renderer,textures,model.latin_font_texture?model.latin_font_texture:font_texture,
               model.latin_font?*model.latin_font:font,s};
    const std::string back=bank_text(model,3909,"Back");
    std::vector<std::pair<std::string,std::string>> footer;
    if(model.screen==VisualScreen::shop)
        footer={{"A",bank_text(model,3913,"Select")},{"Y",bank_text(model,3914,"Buy")+"/"+bank_text(model,3912,"Sell")},
                {"X",tr_text(model,"Description","Описание")},{"B",back}};
    else footer={{"A",bank_text(model,3908,"Ok")},{"B",back},{"X",tr_text(model,"Page","Стр.")}};
    draw_key_hints(p,pl,{outer.x+14*s,outer.y+outer.h-25*s,outer.w-28*s,18*s},muted,footer);
    if(!model.notice.empty()) {
        SDL_Rect toast{outer.x+14*s,outer.y+outer.h-57*s,outer.w-28*s,25*s};
        p.rect(toast,{49,45,31,255});p.outline(toast,gold,s);
        p.text(model.notice,{toast.x+7*s,toast.y+6*s,toast.w-14*s,16*s},gold);
    }
    if(model.confirming) {
        p.rect({0,0,output_w,output_h},{0,0,0,175});
        SDL_Rect modal{output_w/2-190*s,output_h/2-90*s,380*s,180*s};p.frame(modal,true);
        p.text(model.modal_title,{modal.x+18*s,modal.y+17*s,modal.w-36*s,32*s},gold);
        p.text(model.modal_text,{modal.x+18*s,modal.y+60*s,modal.w-36*s,64*s});
        p.button({modal.x+18*s,modal.y+modal.h-44*s,160*s,28*s},bank_text(model,3915,"Yes"),true);
        p.button({modal.x+modal.w-178*s,modal.y+modal.h-44*s,160*s,28*s},bank_text(model,3916,"No"));
    }
}
