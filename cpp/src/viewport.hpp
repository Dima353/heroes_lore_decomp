#pragma once
#include <cstddef>
struct ViewRect { int x{}, y{}, w{}, h{}; };
struct WorldViewport {
    int scale{1}, left{}, top{}, cell{16};
};
struct WorldCamera {
    WorldViewport view;
    bool initialized{};
    void reset() {initialized=false;}
    WorldViewport update(WorldViewport target,int direction,bool smooth,bool tick);
};
WorldViewport calculate_world_viewport(int output_w, int output_h, int map_w, int map_h,
                                       int hero_x_pixels, int hero_y_pixels, int bottom_inset = 0);
struct DialogueLayout {
    int scale{1}, visible_lines{4};
    ViewRect panel, inner;
};
DialogueLayout calculate_dialogue_layout(int output_w, int output_h, int font_height, int portrait = 0);
// 3DS: the dialogue window in the middle of the 320x240 bottom screen, the face inside it.
DialogueLayout calculate_bottom_dialogue_layout(int font_height, int portrait = 0);

std::size_t move_grid_selection(std::size_t selected,std::size_t count,int direction,std::size_t columns=6);
