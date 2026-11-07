#pragma once
#include "map.hpp"
#include <algorithm>
#include <vector>

enum class WorldDrawKind { object, enemy, npc, hero };
struct WorldDrawEntry { int depth; WorldDrawKind kind; std::size_t index; };
// aq.c compares ck.d(short)+ck.d(byte): physical Y plus the anchor offset.
inline std::vector<WorldDrawEntry> world_draw_order(const std::vector<MapEntity>& objects,
    const std::vector<MapEntity>& npcs,const std::vector<MapEntity>& enemies,int hero_y) {
    std::vector<WorldDrawEntry> order;
    order.reserve(objects.size()+npcs.size()+enemies.size()+1);
    for(std::size_t i=0;i<objects.size();++i)
        order.push_back({int(objects[i].y)*16+objects[i].offset_y,WorldDrawKind::object,i});
    for(std::size_t i=0;i<enemies.size();++i)
        order.push_back({int(enemies[i].y)*16+(enemies[i].partial_y?8:0)+8,WorldDrawKind::enemy,i});
    for(std::size_t i=0;i<npcs.size();++i)
        order.push_back({int(npcs[i].y)*16+(npcs[i].partial_y?8:0)+8,WorldDrawKind::npc,i});
    order.push_back({hero_y+8,WorldDrawKind::hero,0});
    std::stable_sort(order.begin(),order.end(),[](const auto& a,const auto& b){return a.depth<b.depth;});
    return order;
}
// ae draws 10 animation steps: two opening, six held, two closing, 22px band.
constexpr int location_banner_height(int step) {
    return step<0 || step>=10 ? 0 : step<2 ? (step+1)*8 : step>=8 ? (10-step)*8 : 22;
}
static_assert(location_banner_height(0)==8 && location_banner_height(3)==22 &&
              location_banner_height(9)==8 && location_banner_height(10)==0);
