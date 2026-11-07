#include "viewport.hpp"
#include <algorithm>
#include <stdexcept>

WorldViewport WorldCamera::update(WorldViewport target,int direction,bool smooth,bool tick) {
    if(!initialized || !smooth || target.scale!=view.scale) {
        view=target;initialized=true;return view;
    }
    if(tick) {
        // n.a(true,true): update only the axis along the current facing.
        const auto approach=[&](int current,int desired) {
            if(current==desired) return current;
            const int a=current/target.scale,b=desired/target.scale;
            return (a+(b-a+1)/2-1)*target.scale;
        };
        if(direction==3 || direction==4) view.left=approach(view.left,target.left);
        else view.top=approach(view.top,target.top);
    }
    return view;
}

WorldViewport calculate_world_viewport(int output_w, int output_h, int map_w, int map_h,
                                       int hero_x_pixels, int hero_y_pixels, int bottom_inset) {
    if (output_w <= 0 || output_h <= 0 || map_w <= 0 || map_h <= 0)
        throw std::invalid_argument("invalid world viewport size");
    WorldViewport view;
    // World magnification depends only on display resolution, never map size.
    view.scale = std::max(1, std::min(output_w / 320, output_h / 240));
    view.cell = 16 * view.scale;
    const auto offset = [&](int output, int tiles, int hero) {
        const int extent = tiles * 16;
        if (extent * view.scale <= output) return (output - extent * view.scale) / 2;
        const int logical_view = (output + view.scale - 1) / view.scale;
        const int camera = std::clamp(hero + 8 - logical_view / 2, 0, extent - logical_view);
        return -camera * view.scale;
    };
    view.left = offset(output_w, map_w, hero_x_pixels);
    view.top = offset(std::max(1,output_h-bottom_inset), map_h, hero_y_pixels);
    return view;
}
DialogueLayout calculate_dialogue_layout(int output_w, int output_h, int font_height, int portrait) {
    if (output_w <= 0 || output_h <= 0 || font_height <= 0)
        throw std::invalid_argument("invalid dialogue viewport size");
    DialogueLayout layout;
    // At least the HUD's scale up to 2: 640x480 handhelds got 1 and tiny text (1280x720 and up keep theirs).
    layout.scale = std::max({1, std::min(output_w / 480, output_h / 270), std::min({output_w / 320, output_h / 240, 2})});
    const int scale = layout.scale;
    const int width = std::max(1,std::min(output_w-16*scale,560*scale));
    const int height = std::min(output_h,std::max(84,font_height*layout.visible_lines+32)*scale);
    // ah draws the dialogue over the bottom HUD, reserving only the softkey line.
    layout.panel = {(output_w-width)/2,std::max(0,output_h-height-10*scale),width,height};
    // Faces sit above the box and do not reduce the original text area.
    (void)portrait;
    layout.inner = {layout.panel.x+12*scale,layout.panel.y+14*scale,
                    std::max(1,width-24*scale),font_height*layout.visible_lines*scale};
    return layout;
}

std::size_t move_grid_selection(std::size_t selected,std::size_t count,int direction,std::size_t columns) {
    if(!count) return 0;
    columns=std::max<std::size_t>(1,columns);selected=std::min(selected,count-1);
    if(direction==1 && selected>=columns) return selected-columns;
    if(direction==2 && selected/columns<(count-1)/columns) return std::min(count-1,selected+columns);
    if(direction==3) return (selected+count-1)%count;
    if(direction==4) return (selected+1)%count;
    return selected;
}
