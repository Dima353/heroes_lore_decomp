#define HL3D_IMPLEMENTATION
#include "gpu_3ds.hpp"

#if defined(HEROES_LORE_3DS_GPU)
#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace hl3d {
namespace {

struct Texture {
    C3D_Tex tex{};
    int w{}, h{}, tw{}, th{};
    Uint32 format{};
    int access{};
    Uint8 r{255}, g{255}, b{255};
    SDL_BlendMode blend{SDL_BLENDMODE_NONE};
    SDL_ScaleMode scale{SDL_ScaleModeNearest};
    bool bottom_target{}; // the 320x240 SDL target texture stands for the bottom screen
};

enum class Kind : Uint8 { image, rect, clear };
struct Command {
    Kind kind{};
    Texture* tex{};
    float u0{}, v0{}, u1{}, v1{};   // texture coordinates (citro2d: v counts from the bottom)
    float x{}, y{}, w{}, h{};       // destination, top-left
    float cx{}, cy{}, angle{};      // rotation centre relative to x,y; radians
    u32 tint{};                      // C2D_Color32, 0xFFFFFFFF = none
    u32 color{};                     // rect / clear colour
    float depth{};
    SDL_Rect clip{};
    bool clipped{};
};
struct Screen {
    int w{}, h{};
    std::vector<Command> commands;
    SDL_Rect clip{};
    bool clipped{};
};

Screen top_screen{400, 240, {}, {}, false};
Screen bottom_screen{320, 240, {}, {}, false};
Screen* current = &top_screen;
C3D_RenderTarget* left_eye = nullptr;
C3D_RenderTarget* right_eye = nullptr;
C3D_RenderTarget* bottom_target = nullptr;
// Stereo field: the ground-plane draws are rendered once into this texture, then drawn per eye.
C3D_Tex field_tex{};
C3D_RenderTarget* field_target = nullptr;
Uint8 draw_r = 0, draw_g = 0, draw_b = 0, draw_a = 255;
float layer_depth = 0.0f;
std::vector<Texture*> released; // freed after the frame that may still draw them
bool ready = false;
// HOME -> close: from then on the GPU belongs to the system and C3D_FrameBegin would wait forever
// (the Homebrew Launcher then hung "closing"); no more frames, no citro3d teardown.
volatile bool exiting = false;
aptHookCookie apt_cookie;
void apt_hook(APT_HookType hook, void*) { if (hook == APTHOOK_ONEXIT) exiting = true; }
constexpr float kMaxShift = 5.0f; // pixels per eye at full slider for depth 1

int pow2(int v) { int p = 8; while (p < v) p <<= 1; return p; }

// 3DS textures are 8x8 tiles in Morton order; texture row r is sampled at v = 1 - r / height
// (checked on hardware: image row y goes to row y, the top of the image at v = 1).
u32 morton_offset(u32 x, u32 y) {
    u32 i = (x & 7) | ((y & 7) << 8);
    i = (i ^ (i << 2)) & 0x1313;
    i = (i ^ (i << 1)) & 0x1515;
    i = (i | (i >> 7)) & 0x3F;
    return i + (x & ~7u) * 8;
}

Texture* as_texture(SDL_Texture* t) { return reinterpret_cast<Texture*>(t); }

void record(const Command& command) { current->commands.push_back(command); }

void apply_clip(const Screen& screen, const Command& command, bool& clip_on, SDL_Rect& clip_now) {
    if (command.clipped == clip_on && (!clip_on || std::memcmp(&clip_now, &command.clip, sizeof(SDL_Rect)) == 0)) return;
    C2D_Flush();
    clip_on = command.clipped;
    clip_now = command.clip;
    if (!clip_on) { C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0); return; }
    // Screens are rotated framebuffers: x runs down the 240-pixel side, y along the long side.
    const int x0 = std::clamp(command.clip.x, 0, screen.w), x1 = std::clamp(command.clip.x + command.clip.w, 0, screen.w);
    const int y0 = std::clamp(command.clip.y, 0, screen.h), y1 = std::clamp(command.clip.y + command.clip.h, 0, screen.h);
    C3D_SetScissor(GPU_SCISSOR_NORMAL, u32(screen.h - y1), u32(screen.w - x1), u32(screen.h - y0), u32(screen.w - x0));
}

void draw_command(const Screen& screen, const Command& command, float shift) {
    if (command.kind == Kind::clear) {
        C2D_DrawRectSolid(0, 0, 0, float(screen.w), float(screen.h), command.color);
    } else if (command.kind == Kind::rect) {
        C2D_DrawRectSolid(command.x + shift, command.y, 0, command.w, command.h, command.color);
    } else if (command.tex) {
        const Tex3DS_SubTexture sub{u16(std::fabs(command.w)), u16(std::fabs(command.h)),
                                    command.u0, command.v0, command.u1, command.v1};
        const C2D_Image image{&command.tex->tex, &sub};
        C2D_DrawParams params{};
        params.pos.x = command.x + command.cx + shift;
        params.pos.y = command.y + command.cy;
        params.pos.w = command.w;
        params.pos.h = command.h;
        params.center.x = command.cx;
        params.center.y = command.cy;
        params.depth = 0;
        params.angle = command.angle;
        if (command.tint == 0xFFFFFFFFu) C2D_DrawImage(image, &params, nullptr);
        else {
            C2D_ImageTint tint;
            C2D_PlainImageTint(&tint, command.tint, 1.0f);
            C2D_DrawImage(image, &params, &tint);
        }
    }
}

// Commands [first, last) at depth >= 0 (all of them when `all`), in order, with their clips.
void draw_range(const Screen& screen, std::size_t first, std::size_t last, float eye, float slider, bool all) {
    bool clip_on = false;
    SDL_Rect clip_now{};
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    for (std::size_t i = first; i < last; ++i) {
        const auto& command = screen.commands[i];
        if (!all && command.depth < 0.0f) continue;
        apply_clip(screen, command, clip_on, clip_now);
        draw_command(screen, command, eye * std::max(0.0f, command.depth) * slider * kMaxShift);
    }
    C2D_Flush();
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
}

// Depth of a field row: 1 at the top of the screen (far), 0.25 at the bottom (near).
float ground_depth(float y, float height) { return std::clamp(0.25f + 0.75f * (1.0f - y / height), 0.0f, 1.0f); }

// The field pass: every ground-plane command (depth < 0), unshifted, into field_tex.
void render_field(const Screen& screen) {
    C2D_TargetClear(field_target, 0);
    C2D_SceneBegin(field_target);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    for (const auto& command : screen.commands) if (command.depth < 0.0f) draw_command(screen, command, 0.0f);
    C2D_Flush();
}

void replay(const Screen& screen, C3D_RenderTarget* target, float eye, float slider, bool field) {
    C2D_TargetClear(target, C2D_Color32(0, 0, 0, 255));
    C2D_SceneBegin(target);
    std::size_t first_field = screen.commands.size();
    if (field) for (std::size_t i = 0; i < screen.commands.size(); ++i)
        if (screen.commands[i].depth < 0.0f) {first_field = i; break;}
    if (first_field == screen.commands.size()) {draw_range(screen, 0, screen.commands.size(), eye, slider, true); return;}
    draw_range(screen, 0, first_field, eye, slider, false);                      // under the field
    // The field as one-pixel rows, each shifted by its own fractional amount (linear filtering):
    // whole-pixel shifts per draw made a tilted plane of 4-5 visible bands.
    for (int row = 0; row < screen.h; ++row) {
        const float shift = eye * ground_depth(float(row) + 1.0f, float(screen.h)) * slider * kMaxShift;
        const Tex3DS_SubTexture sub{u16(screen.w), 1, 0.0f, 1.0f - float(row) / 256.0f,
                                    float(screen.w) / 512.0f, 1.0f - float(row + 1) / 256.0f};
        C2D_DrawImageAt(C2D_Image{&field_tex, &sub}, shift, float(row), 0.0f);
    }
    C2D_Flush();
    draw_range(screen, first_field, screen.commands.size(), eye, slider, false); // over it: the interface
}

} // namespace

bool init() {
    if (ready) return true;
    // SDL's N3DS video set both screens to RGBA8; citro2d's screen targets transfer BGR8 (3 bytes a
    // pixel), which read as RGBA8 squeezed the picture into 3/4 of the width in stripes.
    gfxSetScreenFormat(GFX_TOP, GSP_BGR8_OES);
    gfxSetScreenFormat(GFX_BOTTOM, GSP_BGR8_OES);
    if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE)) return false;
    if (!C2D_Init(12288)) return false; // both eyes plus the bottom screen per frame
    C2D_Prepare();
    C2D_SetTintMode(C2D_TintMult);       // SDL colour mod multiplies
    gfxSet3D(true);
    left_eye = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    right_eye = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
    bottom_target = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    if (C3D_TexInitVRAM(&field_tex, 512, 256, GPU_RGBA8)) {
        C3D_TexSetFilter(&field_tex, GPU_LINEAR, GPU_LINEAR); // fractional row shifts
        field_target = C3D_RenderTargetCreateFromTex(&field_tex, GPU_TEXFACE_2D, 0, -1);
    }
    ready = left_eye && right_eye && bottom_target && field_target;
    if (ready) aptHook(&apt_cookie, apt_hook, nullptr);
    return ready;
}

void shutdown() {
    if (!ready) return;
    aptUnhook(&apt_cookie);
    if (exiting) {ready = false; return;}
    C2D_Fini();
    C3D_Fini();
    ready = false;
}

void end_frame() {
    if (!ready) return;
    if (closing()) {top_screen.commands.clear(); bottom_screen.commands.clear(); return;}
    const float slider = osGet3DSliderState();
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    // Left eye sees far layers shifted left, the right eye right: depth > 0 sits behind the screen.
    render_field(top_screen);
    replay(top_screen, left_eye, -1.0f, slider, true);
    replay(top_screen, right_eye, 1.0f, slider, true);
    replay(bottom_screen, bottom_target, 0.0f, 0.0f, false);
    C3D_FrameEnd(0);
    top_screen.commands.clear();
    bottom_screen.commands.clear();
    top_screen.clipped = bottom_screen.clipped = false;
    for (auto* texture : released) {C3D_TexDelete(&texture->tex); delete texture;}
    released.clear();
}

// APTHOOK_ONEXIT only fires in aptExit, at the very end; aptShouldClose() is set as soon as HOME
// asks the app to close (SDL then sends SDL_QUIT).
bool closing() { if (aptShouldClose()) exiting = true; return exiting; }
void set_depth(float depth) { layer_depth = depth; }
void target_bottom(bool bottom) { current = bottom ? &bottom_screen : &top_screen; }

SDL_Texture* create_texture(SDL_Renderer*, Uint32 format, int access, int w, int h) {
    auto* texture = new Texture;
    texture->w = w; texture->h = h; texture->format = format; texture->access = access;
    if (access == SDL_TEXTUREACCESS_TARGET) { texture->bottom_target = true; return reinterpret_cast<SDL_Texture*>(texture); }
    texture->tw = pow2(w); texture->th = pow2(h);
    if (texture->tw > 1024 || texture->th > 1024 ||
        !C3D_TexInit(&texture->tex, u16(texture->tw), u16(texture->th), GPU_RGBA8)) {
        delete texture;
        SDL_SetError("3DS texture %dx%d unavailable", w, h);
        return nullptr;
    }
    std::memset(texture->tex.data, 0, texture->tex.size);
    C3D_TexSetFilter(&texture->tex, GPU_NEAREST, GPU_NEAREST);
    C3D_TexSetWrap(&texture->tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    return reinterpret_cast<SDL_Texture*>(texture);
}

int update_texture(SDL_Texture* handle, const SDL_Rect* rect, const void* pixels, int pitch) {
    auto* texture = as_texture(handle);
    if (!texture || texture->bottom_target || !pixels) return -1;
    const SDL_Rect area = rect ? *rect : SDL_Rect{0, 0, texture->w, texture->h};
    auto* out = static_cast<u32*>(texture->tex.data);
    const bool argb = texture->format == SDL_PIXELFORMAT_ARGB8888;
    for (int y = 0; y < area.h; ++y) {
        const auto* row = reinterpret_cast<const Uint8*>(pixels) + std::ptrdiff_t(y) * pitch;
        const u32 ty = u32(area.y + y);
        for (int x = 0; x < area.w; ++x) {
            u32 value;
            if (argb) {
                u32 p; std::memcpy(&p, row + x * 4, 4);
                value = ((p & 0x00FFFFFFu) << 8) | (p >> 24);         // 0xAARRGGBB -> 0xRRGGBBAA
            } else {
                const Uint8* p = row + x * 4;                         // R, G, B, A bytes
                value = (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
            }
            const u32 tx = u32(area.x + x);
            out[(ty & ~7u) * u32(texture->tw) + morton_offset(tx, ty)] = value;
        }
    }
    C3D_TexFlush(&texture->tex);
    return 0;
}

int query_texture(SDL_Texture* handle, Uint32* format, int* access, int* w, int* h) {
    auto* texture = as_texture(handle);
    if (!texture) return -1;
    if (format) *format = texture->format;
    if (access) *access = texture->access;
    if (w) *w = texture->w;
    if (h) *h = texture->h;
    return 0;
}

void destroy_texture(SDL_Texture* handle) {
    auto* texture = as_texture(handle);
    if (!texture) return;
    if (texture->bottom_target) {delete texture; return;}
    released.push_back(texture);
}

int set_texture_color_mod(SDL_Texture* handle, Uint8 r, Uint8 g, Uint8 b) {
    auto* texture = as_texture(handle);
    if (!texture) return -1;
    texture->r = r; texture->g = g; texture->b = b;
    return 0;
}
int set_texture_blend_mode(SDL_Texture* handle, SDL_BlendMode mode) {
    if (auto* texture = as_texture(handle)) {texture->blend = mode; return 0;}
    return -1;
}
int set_texture_scale_mode(SDL_Texture* handle, SDL_ScaleMode mode) {
    auto* texture = as_texture(handle);
    if (!texture || texture->bottom_target) return -1;
    texture->scale = mode;
    const GPU_TEXTURE_FILTER_PARAM filter = mode == SDL_ScaleModeNearest ? GPU_NEAREST : GPU_LINEAR;
    C3D_TexSetFilter(&texture->tex, filter, filter);
    return 0;
}
int get_texture_scale_mode(SDL_Texture* handle, SDL_ScaleMode* mode) {
    auto* texture = as_texture(handle);
    if (!texture || !mode) return -1;
    *mode = texture->scale;
    return 0;
}

int render_copy_ex(SDL_Renderer*, SDL_Texture* handle, const SDL_Rect* src, const SDL_Rect* dst, double angle,
                   const SDL_Point* center, SDL_RendererFlip flip) {
    auto* texture = as_texture(handle);
    if (!texture || texture->bottom_target) return -1;
    const SDL_Rect s = src ? *src : SDL_Rect{0, 0, texture->w, texture->h};
    const SDL_Rect d = dst ? *dst : SDL_Rect{0, 0, current->w, current->h};
    if (s.w <= 0 || s.h <= 0 || d.w <= 0 || d.h <= 0) return 0;
    Command command;
    command.kind = Kind::image;
    command.tex = texture;
    const float tw = float(texture->tw), th = float(texture->th);
    command.u0 = float(s.x) / tw; command.u1 = float(s.x + s.w) / tw;
    command.v0 = 1.0f - float(s.y) / th; command.v1 = 1.0f - float(s.y + s.h) / th;
    if (flip & SDL_FLIP_HORIZONTAL) std::swap(command.u0, command.u1);
    if (flip & SDL_FLIP_VERTICAL) std::swap(command.v0, command.v1);
    command.x = float(d.x); command.y = float(d.y); command.w = float(d.w); command.h = float(d.h);
    if (angle != 0.0) {
        command.cx = center ? float(center->x) : float(d.w) * 0.5f;
        command.cy = center ? float(center->y) : float(d.h) * 0.5f;
        command.angle = float(angle * 3.14159265358979323846 / 180.0);
    }
    command.tint = (texture->r == 255 && texture->g == 255 && texture->b == 255) ? 0xFFFFFFFFu
                                                                                 : C2D_Color32(texture->r, texture->g, texture->b, 255);
    command.depth = layer_depth;
    command.clip = current->clip; command.clipped = current->clipped;
    record(command);
    return 0;
}
int render_copy(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_Rect* src, const SDL_Rect* dst) {
    return render_copy_ex(renderer, texture, src, dst, 0.0, nullptr, SDL_FLIP_NONE);
}

int set_draw_color(SDL_Renderer*, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {draw_r = r; draw_g = g; draw_b = b; draw_a = a; return 0;}
int set_draw_blend_mode(SDL_Renderer*, SDL_BlendMode) { return 0; } // fills use their alpha either way

int fill_rect(SDL_Renderer*, const SDL_Rect* rect) {
    Command command;
    command.kind = Kind::rect;
    const SDL_Rect r = rect ? *rect : SDL_Rect{0, 0, current->w, current->h};
    if (r.w <= 0 || r.h <= 0) return 0;
    command.x = float(r.x); command.y = float(r.y); command.w = float(r.w); command.h = float(r.h);
    command.color = C2D_Color32(draw_r, draw_g, draw_b, draw_a);
    command.depth = layer_depth;
    command.clip = current->clip; command.clipped = current->clipped;
    record(command);
    return 0;
}
int draw_rect(SDL_Renderer* renderer, const SDL_Rect* rect) {
    if (!rect) return -1;
    const SDL_Rect edges[4]{{rect->x, rect->y, rect->w, 1}, {rect->x, rect->y + rect->h - 1, rect->w, 1},
                            {rect->x, rect->y, 1, rect->h}, {rect->x + rect->w - 1, rect->y, 1, rect->h}};
    for (const auto& edge : edges) fill_rect(renderer, &edge);
    return 0;
}
int draw_line(SDL_Renderer* renderer, int x1, int y1, int x2, int y2) {
    // Only axis-aligned lines are drawn by the game; anything else becomes its bounding strip.
    const SDL_Rect r{std::min(x1, x2), std::min(y1, y2), std::abs(x2 - x1) + 1, std::abs(y2 - y1) + 1};
    return fill_rect(renderer, &r);
}
int clear(SDL_Renderer*) {
    current->commands.clear(); // everything before is covered anyway
    Command command;
    command.kind = Kind::clear;
    command.color = C2D_Color32(draw_r, draw_g, draw_b, 255);
    record(command);
    return 0;
}

int set_clip_rect(SDL_Renderer*, const SDL_Rect* rect) {
    current->clipped = rect != nullptr;
    current->clip = rect ? *rect : SDL_Rect{};
    return 0;
}
void get_clip_rect(SDL_Renderer*, SDL_Rect* rect) { if (rect) *rect = current->clipped ? current->clip : SDL_Rect{}; }
SDL_bool is_clip_enabled(SDL_Renderer*) { return current->clipped ? SDL_TRUE : SDL_FALSE; }
void get_viewport(SDL_Renderer*, SDL_Rect* rect) { if (rect) *rect = SDL_Rect{0, 0, current->w, current->h}; }
int set_scale(SDL_Renderer*, float, float) { return 0; }
int get_output_size(SDL_Renderer*, int* w, int* h) {
    if (w) *w = current->w;
    if (h) *h = current->h;
    return 0;
}
void present(SDL_Renderer*) {} // hl3d::end_frame() presents both screens
int read_pixels(SDL_Renderer*, const SDL_Rect*, Uint32, void*, int) { return SDL_SetError("read-back unavailable on the 3DS GPU path"); }
int flush(SDL_Renderer*) { return 0; }
int set_render_target(SDL_Renderer*, SDL_Texture* handle) {
    target_bottom(handle != nullptr);
    return 0;
}
int get_renderer_info(SDL_Renderer*, SDL_RendererInfo* info) {
    if (!info) return -1;
    *info = SDL_RendererInfo{};
    info->name = "citro2d";
    info->flags = SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE;
    info->max_texture_width = info->max_texture_height = 1024;
    return 0;
}

} // namespace hl3d
#endif
