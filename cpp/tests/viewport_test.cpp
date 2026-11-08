#include "viewport.hpp"
#include <iostream>
#include <stdexcept>
static void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main() {
    try {
        const auto town = calculate_world_viewport(1280,720,28,32,352,64);
        const auto house = calculate_world_viewport(1280,720,20,20,160,288);
        const auto huge = calculate_world_viewport(1280,720,100,100,800,800);
        check(town.scale == 3 && house.scale == 3 && huge.scale == 3 && town.cell == house.cell,
              "map dimensions must not change character magnification");
        const auto hud_view=calculate_world_viewport(1280,720,100,100,800,800,63);
        check(hud_view.scale==town.scale && hud_view.top<huge.top,
              "HUD reservation shifts camera without changing actor scale");
        check(house.left == (1280-20*48)/2, "small map centered horizontally");
        check(huge.left < 0 && huge.top < 0, "large map follows hero");
        check(huge.left + (800+8)*3 >= 1280/2-3 && huge.left+(800+8)*3 <= 1280/2+3,
              "hero stays near viewport center away from map edge");
        const auto top_left = calculate_world_viewport(1280,720,100,100,0,0);
        check(top_left.left == 0 && top_left.top == 0, "camera clamps at top-left edge");
        const auto bottom_right = calculate_world_viewport(1280,720,100,100,1584,1584);
        check(bottom_right.left+100*48 >= 1280 && bottom_right.left+100*48 <= 1283 &&
              bottom_right.top+100*48 == 720, "camera clamps at bottom-right edge");
        for (const auto size : {ViewRect{0,0,960,720}, ViewRect{0,0,1280,720}, ViewRect{0,0,1920,1080}}) {
            const auto layout = calculate_dialogue_layout(size.w,size.h,10);
            check(layout.panel.x >= 0 && layout.panel.y >= 0 &&
                  layout.panel.x+layout.panel.w <= size.w && layout.panel.y+layout.panel.h <= size.h,
                  "dialogue panel fits display");
            check(layout.inner.h == layout.visible_lines*10*layout.scale,
                  "render and pagination use the same four-line area");
            check(layout.inner.x > layout.panel.x &&
                  layout.inner.x+layout.inner.w < layout.panel.x+layout.panel.w,
                  "text remains inside decorative border");
        }
        const auto plain=calculate_dialogue_layout(1280,720,10,0);
        const auto left=calculate_dialogue_layout(1280,720,10,2);
        const auto right=calculate_dialogue_layout(1280,720,10,-2);
        check(left.inner.w==right.inner.w && left.inner.w==plain.inner.w &&
              left.inner.x==right.inner.x, "portrait above panel preserves full text width");
        for(const int face:{0,2,-2}) { // 3DS bottom screen: face inside the window, beside the text
            const auto below=calculate_bottom_dialogue_layout(12,face);
            check(below.panel.x>=0 && below.panel.x+below.panel.w<=320 && below.panel.y>=30 &&
                  below.panel.y+below.panel.h<=152,"bottom dialogue fits between the enemy bar and the HP row");
            check(below.inner.h==below.visible_lines*12 && below.inner.y+below.inner.h<=below.panel.y+below.panel.h-4,
                  "bottom dialogue pages fit the window");
            const int face_left=face>0?below.panel.x+5:below.panel.x+below.panel.w-55;
            check(face==0 || below.inner.x>=face_left+50 || below.inner.x+below.inner.w<=face_left,
                  "bottom dialogue text clears the face");
        }
        check(move_grid_selection(0,6,2)==0,"down on a single row keeps the current slot");
        check(move_grid_selection(0,7,2)==6 && move_grid_selection(6,7,1)==0,"move between grid rows");
        check(move_grid_selection(0,6,3)==5 && move_grid_selection(5,6,4)==0,"horizontal wrap");
        check(move_grid_selection(0,0,2)==0,"empty grid");
        for(std::size_t count=1;count<=30;++count) for(std::size_t at=0;at<count;++at)
            for(int dir=1;dir<=4;++dir) check(move_grid_selection(at,count,dir)<count,"grid selection remains valid");
        std::cout << "Viewport checks passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
