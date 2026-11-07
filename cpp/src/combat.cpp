#if defined(HEROES_LORE_PSP_BUILD)
#include "psp_diagnostics.hpp"
#endif
#include "combat.hpp"

#include <algorithm>
#include <cstdlib>
#include <stdexcept>

namespace combat {
namespace {

int s16(const std::vector<std::uint8_t>& d, std::size_t pos) {
    if (pos + 1 >= d.size()) throw std::runtime_error("truncated enemy table");
    return static_cast<std::int16_t>((d[pos] << 8) | d[pos + 1]);
}
int i8(std::uint8_t v) { return static_cast<std::int8_t>(v); }

// ao.b/c/d: frames per combo step, row 0 normal, row 1 skill.
constexpr int kComboFrames[3][2][5] = {
    {{4, 4, 4, 4, 0}, {4, 0, 4, 4, 8}},
    {{3, 3, 3, 6, 0}, {4, 0, 7, 9, 14}},
    {{3, 3, 3, 3, 0}, {3, 0, 3, 3, 6}},
};

} // namespace

std::vector<EnemyStats> parse_enemy_table(const std::vector<std::uint8_t>& d) {
    // j.a([BBB): records start at byte 1, each prefixed by a big-endian length.
    std::vector<EnemyStats> out;
    std::size_t pos = 1;
    while (pos + 1 < d.size()) {
        const auto length = static_cast<std::size_t>(s16(d, pos));
        const std::size_t end = pos + 2 + length;
        if (end > d.size()) throw std::runtime_error("enemy record exceeds table");
        std::size_t p = pos + 3; // length + one skipped byte
        EnemyStats s;
        const std::size_t name_length = d.at(p++);
        if (p + name_length > end) throw std::runtime_error("enemy name exceeds record");
        s.name_text = std::atoi(std::string(d.begin() + static_cast<std::ptrdiff_t>(p),
                                            d.begin() + static_cast<std::ptrdiff_t>(p + name_length)).c_str());
        p += name_length;
        int b = d.at(p++);
        s.size = (b >> 6) & 3; s.respawn = (b >> 4) & 3; s.element = (b >> 2) & 3; s.attack_kind = b & 3;
        b = d.at(p++);
        s.chase_far = (b >> 3) & 1; s.half_damage = (b >> 2) & 1; s.summoner = (b >> 1) & 1; s.ambush = b & 1;
        if (s.summoner) s.summon_element = (b >> 6) & 3;
        s.level = i8(d.at(p++));
        s.hp = s16(d, p); p += 2;
        s.attack = s16(d, p); p += 2;
        s.defense = s16(d, p); p += 2;
        s.evasion = s16(d, p); p += 2;
        s.aggro_range = i8(d.at(p++));
        s.move_delay = i8(d.at(p++));
        s.attack_delay = i8(d.at(p++));
        s.respawn_delay = s16(d, p); p += 2;
        const std::size_t drops = d.at(p++);
        if (p + drops * 3 != end) throw std::runtime_error("enemy drop table size mismatch");
        for (std::size_t i = 0; i < drops; ++i, p += 3)
            s.drops.push_back({i8(d[p]), i8(d[p + 1]), i8(d[p + 2])});
        out.push_back(std::move(s));
        pos = end;
    }
    return out;
}

void CombatWorld::reset(int width, int height, std::vector<std::int8_t> overlay,
                        const std::vector<EnemyStats>* stats, std::uint32_t seed) {
    if (width <= 0 || height <= 0 || overlay.size() != std::size_t(width) * std::size_t(height))
        throw std::invalid_argument("combat map size mismatch");
    width_ = width; height_ = height; overlay_ = std::move(overlay); stats_ = stats;
    occupancy_.assign(overlay_.size(), 0);
    enemies_.clear(); spawns_.clear(); drops_.clear(); popups_.clear(); projectiles_.clear();
    npc_tiles_.clear();
    spawn_timer_ = 0;
    initial_spawns_ = true;
    rng_ = (std::uint64_t(seed) ^ 0x5DEECE66DULL) & ((1ULL << 48) - 1);
    clear_hero();
    recover_ = dash_ = 0;
    effects_.clear();
    battle_effects_.clear();target_=-1;target_ticks_=0;
    auras_.clear();
    guardian_ = FieldGuardian{};
    // These flags belong to the cancelled summon, not to a timed cf status.
    // Keep poison/attribute timers and cooldowns on ordinary map changes.
    status_.thorns = status_.regen_double = status_.invulnerable = false;
}

void CombatWorld::set_enemy_frames(int sprite, EnemyFrames frames) {
    if (sprite >= 0 && std::size_t(sprite) < frames_.size()) frames_[std::size_t(sprite)] = frames;
}

int CombatWorld::next_int() { // java.util.Random.nextInt()
    rng_ = (rng_ * 0x5DEECE66DULL + 0xBULL) & ((1ULL << 48) - 1);
    return static_cast<std::int32_t>(rng_ >> 16);
}
int CombatWorld::rand_range(int lo, int hi) {
    const int span = hi - lo + 1;
    if (span <= 0) return 0;
    return lo + std::abs(next_int()) % span;
}

void CombatWorld::queue_spawn(int sprite, int stats_index, int x, int y, int delay) {
    spawns_.push_back({delay, sprite, stats_index, x, y});
}

bool CombatWorld::tile_free(int x, int y) const {
    if (!in_bounds(x, y)) return false;
    const auto i = std::size_t(y) * std::size_t(width_) + std::size_t(x);
    return overlay_[i] >= 0 && occupancy_[i] == 0;
}
int CombatWorld::occupant(int x, int y) const {
    return in_bounds(x, y) ? occupancy_[std::size_t(y) * std::size_t(width_) + std::size_t(x)] : 0;
}
bool CombatWorld::can_place(int self, int width, int x, int y) const {
    for (int i = 0; i < width; ++i) {
        if (!in_bounds(x + i, y)) return false;
        if (!tile_free(x + i, y) && occupant(x + i, y) != self) return false;
    }
    return true;
}
bool CombatWorld::can_step(int self, int direction) const {
    const auto& e = enemies_[std::size_t(self - 1)];
    return can_place(self, e.width, e.tile_x + kDx[std::size_t(direction)], e.tile_y + kDy[std::size_t(direction)]);
}
bool CombatWorld::occupied_by_enemy(int x, int y) const { return occupant(x, y) > 0; }

void CombatWorld::unregister(int index) { // o.f
    const auto& e = enemies_[std::size_t(index)];
    if (e.ghost) return;
    const auto clear = [&](int x, int y) { if (in_bounds(x, y)) occupancy_[std::size_t(y) * std::size_t(width_) + std::size_t(x)] = 0; };
    for (int i = 0; i < e.width; ++i) {
        clear(e.tile_x + i, e.tile_y);
        if (e.partial_y) clear(e.tile_x + i, e.tile_y + 1);
        else if (e.partial_x) clear(e.tile_x + 1 + i, e.tile_y);
    }
}
void CombatWorld::register_actor(int index) { // o.g
    const auto& e = enemies_[std::size_t(index)];
    if (e.ghost) return;
    const auto set = [&](int x, int y) { if (in_bounds(x, y)) occupancy_[std::size_t(y) * std::size_t(width_) + std::size_t(x)] = std::int16_t(index + 1); };
    for (int i = 0; i < e.width; ++i) {
        set(e.tile_x + i, e.tile_y);
        if (e.partial_y) set(e.tile_x + i, e.tile_y + 1);
        else if (e.partial_x) set(e.tile_x + 1 + i, e.tile_y);
    }
}

void CombatWorld::rebuild_occupancy(const HeroCombatView& hero) {
    std::fill(occupancy_.begin(), occupancy_.end(), std::int16_t{0});
    const auto mark = [&](int x, int y, std::int16_t id) {
        if (in_bounds(x, y)) occupancy_[std::size_t(y) * std::size_t(width_) + std::size_t(x)] = id;
    };
    for (const auto& tile : npc_tiles_) mark(tile.first, tile.second, -2);
    for (std::size_t i = 0; i < enemies_.size(); ++i) {
        const auto& e = enemies_[i];
        if (e.state == EnemyState::dead) continue;
        register_actor(int(i));
        if (e.boss == BossKind::cg && e.body_row >= 0) // cg.a(ao,ae,byte): tongue over columns 6..9
            for (int x = 6; x <= 9; ++x) mark(x, e.body_row, std::int16_t(i + 1));
    }
    mark(hero.x_tile, hero.y_tile, -1);
    if (hero.partial_y) mark(hero.x_tile, hero.y_tile + 1, -1);
    else if (hero.partial_x) mark(hero.x_tile + 1, hero.y_tile, -1);
}

int CombatWorld::occupant_at(const Enemy& e, int direction, int distance) const { // ck.a(byte,byte)
    switch (direction) {
        case 1: return occupant(e.tile_x, e.tile_y - distance);
        case 2: return occupant(e.tile_x, e.tile_y + distance);
        case 3: return occupant(e.tile_x - distance, e.tile_y);
        case 4: return occupant(e.tile_x + distance, e.tile_y);
        default: return 0;
    }
}

// o.a(byte,ck): the row in front of every footprint column, skipping the actor's own cells.
bool CombatWorld::melee_reaches_hero(std::size_t index) const {
    const auto& e = enemies_[index];
    const int dir = std::clamp(e.heading(), 0, 4);
    for (int i = 0; i < e.width; ++i) {
        const int who = occupant(e.tile_x + kDx[std::size_t(dir)] + i, e.tile_y + kDy[std::size_t(dir)]);
        if (who == int(index) + 1) continue;
        if (who == -1) return true;
    }
    return false;
}

void CombatWorld::move_enemy(Enemy& e, int index, int pixels) { // o.a(int)
    unregister(index);
    switch (e.heading()) {
        case 1: e.y_px -= pixels; if (e.partial_y) e.partial_y = false; else { e.partial_y = true; --e.tile_y; } break;
        case 2: e.y_px += pixels; if (e.partial_y) { e.partial_y = false; ++e.tile_y; } else e.partial_y = true; break;
        case 3: e.x_px -= pixels; if (e.partial_x) e.partial_x = false; else { e.partial_x = true; --e.tile_x; } break;
        case 4: e.x_px += pixels; if (e.partial_x) { e.partial_x = false; ++e.tile_x; } else e.partial_x = true; break;
        default: break;
    }
    if (pixels != 8) {
        e.tile_x = e.x_px >> 4; e.tile_y = e.y_px >> 4;
        e.partial_x = (e.x_px & 15) != 0; e.partial_y = (e.y_px & 15) != 0;
    }
    register_actor(index);
}

bool CombatWorld::spawn_enemy(int x, int y, int sprite, int stats_index, bool scatter, int radius, int attempts) {
    // ae.a(byte,byte,byte,byte,boolean,byte,byte)
    if (!stats_ || stats_index < 0 || std::size_t(stats_index) >= stats_->size()) return false;
    const int width = (*stats_)[std::size_t(stats_index)].size == 2 ? 2 : 1;
    const auto free_run = [&](int px, int py) { for (int i = 0; i < width; ++i) if (!tile_free(px + i, py)) return false; return true; };
    if (scatter) { x += rand_range(-radius, radius); y += rand_range(-radius, radius); }
    while (!free_run(x, y) && attempts > 0) {
        --attempts;
        x += rand_range(-radius, radius); y += rand_range(-radius, radius);
    }
    if (attempts <= 0) return false;
    Enemy e;
    const auto& s = (*stats_)[std::size_t(stats_index)];
    e.sprite = sprite; e.stats = stats_index;
    e.x_px = x * 16; e.y_px = y * 16; e.tile_x = x; e.tile_y = y;
    e.origin_x = x; e.origin_y = y;
    e.width = width; e.hp = s.hp;
    e.move_delay = s.move_delay;
    e.move_cooldown = s.move_delay; e.attack_cooldown = s.attack_delay;
    e.hidden = s.ambush;
    e.state = EnemyState::idle; e.direction = 2;
    e.generation = ++next_generation_;
    // Reuse a dead slot so indexes stay small.
    std::size_t slot = enemies_.size();
    for (std::size_t i = 0; i < enemies_.size(); ++i)
        if (enemies_[i].state == EnemyState::dead && enemies_[i].boss == BossKind::none) { slot = i; break; }
    if (slot == enemies_.size()) enemies_.push_back(e); else enemies_[slot] = e;
    if(target_==int(slot)) {target_=-1;target_ticks_=0;}
    register_actor(int(slot));
    return true;
}

// bu.run case 2 -> ae.d: g=0 and a(false,3), so the map's enemies stand on their own
// tiles (scatter only after a blocked try) before the first ae.b.
void CombatWorld::place_initial_spawns(const HeroCombatView& hero) {
    if (!initial_spawns_) return;
    initial_spawns_ = false;
    rebuild_occupancy(hero);
    spawn_timer_ = 0;
    update_spawns(false);
}

void CombatWorld::update_spawns(bool scatter) { // ae.a(boolean,byte): checked every 16 ticks
    if (--spawn_timer_ >= 0) return;
    spawn_timer_ = 16;
    for (int i = int(spawns_.size()) - 1; i >= 0; --i) {
        auto& sp = spawns_[std::size_t(i)];
        sp.delay -= 16;
        if (sp.delay >= 0) continue;
        const auto& s = (*stats_)[std::size_t(sp.stats)];
        if (s.size == 2 && sp.x >= width_ - 1) { // "INVALID location for enemy - delayed creation."
            Spawn moved = sp; moved.delay = 0; moved.x = sp.x - 1;
            spawns_.erase(spawns_.begin() + i);
            spawns_.push_back(moved);
            continue;
        }
        if (spawn_enemy(sp.x, sp.y, sp.sprite, sp.stats, scatter, 3, 5)) spawns_.erase(spawns_.begin() + i);
        else sp.delay = 0;
    }
}

void CombatWorld::enemy_rest(Enemy& e, bool randomize) { // al.a(boolean)
    const auto& s = stat(e);
    e.move_cooldown = e.move_delay + e.wander;
    e.wander = 0;
    e.attack_cooldown = s.attack_delay*(e.status_elapsed[1]>=0?2:1)+1;
    if (randomize) e.attack_cooldown = e.attack_cooldown * rand_range(1, 7) / 10;
    e.state = EnemyState::idle;
    e.frame = 0;
}
void CombatWorld::enemy_start_attack(Enemy& e) { // al.q
    const auto& s = stat(e);
    e.hidden = false;
    e.move_cooldown = e.move_delay + e.wander;
    e.wander = 0;
    e.attack_cooldown = s.attack_delay*(e.status_elapsed[1]>=0?2:1)+1;
    e.state = EnemyState::attack;
    e.frame = 0;
}
void CombatWorld::enemy_wander(Enemy& e) { // al.r: a(2) keeps o.k; otherwise a(true) rests
    if (next_int() > 0) e.state = EnemyState::walk; else enemy_rest(e, true);
    e.direction = (next_int() & 255) % 4 + 1; e.knock_direction = 0;
}

int CombatWorld::chase_side(std::size_t index, int direction, bool alt) { // o.a(ck,byte,boolean)
    const auto& e = enemies_[index];
    const auto& side = alt ? kSideF : kSideE;
    const auto& diag = alt ? kDiagD : kDiagC;
    const int span = (direction == 1 || direction == 2) ? e.width : 1;
    const int self = int(index) + 1;
    const int s = side[std::size_t(direction)], g = diag[std::size_t(direction)];
    for (int o = -span + 1; o < span; ++o) {
        if (!can_place(self, e.width, e.tile_x + o + kDx[std::size_t(s)], e.tile_y + kDy[std::size_t(s)])) continue;
        const int gx = e.tile_x + o + kDx[std::size_t(g)], gy = e.tile_y + kDy[std::size_t(g)];
        if (can_place(self, e.width, gx, gy) || occupant(gx, gy) == -1) return s;
    }
    return 0;
}

void CombatWorld::enemy_chase(std::size_t index, int range, int target_x, int target_y, bool hero_target) { // o.a(ck,byte)
    auto& e = enemies_[index];
    const int self = int(index) + 1;
    int dx = 100;
    for (int i = 0; i < e.width; ++i) {
        const int d = target_x - (e.tile_x + i);
        if (std::abs(dx) > std::abs(d)) dx = d;
    }
    const int dy = target_y - e.tile_y;
    const int ax = std::abs(dx), ay = std::abs(dy);
    const int random = next_int();
    const bool facing_target = hero_target ? occupant_at(e, e.direction, 1) == -1 :
        e.tile_x + kDx[std::size_t(std::clamp(e.direction, 0, 4))] == target_x && e.tile_y + kDy[std::size_t(std::clamp(e.direction, 0, 4))] == target_y;
    if ((ax + ay <= range && ax * ay == 0) || facing_target) {
        e.direction = dy != 0 ? (dy < 0 ? 1 : 2) : (dx < 0 ? 3 : 4); e.knock_direction = 0;
        return;
    }
    int want;
    if (ay == ax) {
        const int h = dx > 0 ? 4 : 3, v = dy > 0 ? 2 : 1;
        const bool ch = can_step(self, h), cv = can_step(self, v);
        want = (ch && cv) ? (next_int() > 0 ? h : v) : (ch ? h : v);
    } else if (ay > ax) want = dy > 0 ? 2 : 1;
    else want = dx > 0 ? 4 : 3;
    if ((ax <= range || ay <= range) && ax != ay) {
        if (ax <= range && ay < ax) {
            if (dy > 0 && can_step(self, 2)) want = 2;
            else if (dy < 0 && can_step(self, 1)) want = 1;
        } else if (ay <= range && ay > ax) {
            if (dx > 0 && can_step(self, 4)) want = 4;
            else if (dx < 0 && can_step(self, 3)) want = 3;
        }
    }
    int chosen = 0;
    if (can_step(self, want)) chosen = want;
    else {
        bool alt = !((want == 1 && dx > 0) || (want == 2 && dx < 0) || (want == 3 && dy < 0) || (want == 4 && dy > 0));
        chosen = chase_side(index, want, alt);
        if (!chosen) chosen = chase_side(index, want, !alt);
        if (!chosen && alt && can_step(self, kSideF[std::size_t(want)])) chosen = kSideF[std::size_t(want)];
        if (!chosen && !alt && can_step(self, kSideE[std::size_t(want)])) chosen = kSideE[std::size_t(want)];
    }
    if (!chosen) chosen = (random & 255) % 4 + 1;
    e.state = EnemyState::walk;
    e.direction = chosen; e.knock_direction = 0;
}

void CombatWorld::enemy_think(std::size_t index, HeroCombatView& hero) { // al.i
    auto& e = enemies_[index];
    const auto& s = stat(e);
    if (e.attack_cooldown == 0) {
        if (s.attack_kind <= 1) {
            if (melee_reaches_hero(index)) { enemy_start_attack(e); return; }
        } else {
            for (int d = 1; d <= 3; ++d)
                if (occupant_at(e, e.heading(), d) == -1) { enemy_start_attack(e); return; }
        }
    }
    if (e.move_cooldown != 0) return;
    const int range = s.attack_kind >= 2 ? 3 : 1;
    const auto& g = guardian_;
    const bool channelling = g.state == 2; // p.i == 2
    // al.i: keep a channelling guardian; keep the hero unless the summon is in its first ten
    // loops (p.a()Z: k-l < 10, also true before any summon); otherwise ask al.a(ao,p) again.
    if (e.target == 2 && channelling) { enemy_chase(index, range, g.x, g.y, false); return; }
    if (e.target == 1 && g.loops_total - g.loops >= 10) { enemy_chase(index, range, hero_x_, hero_y_, true); return; }
    const int aggro = e.provoked ? (s.chase_far ? 100 : 8) : s.aggro_range;
    const int ax = std::abs(hero.x_tile - e.tile_x), ay = std::abs(hero.y_tile - e.tile_y);
    const bool guardian_near = std::abs(g.x - e.tile_x) <= aggro && std::abs(g.y - e.tile_y) <= aggro;
    if (guardian_near && channelling && rand_range(0, 9) < 7) { enemy_chase(index, range, g.x, g.y, false); e.target = 2; return; }
    if (ax <= aggro && ay <= aggro) { enemy_chase(index, range, hero_x_, hero_y_, true); e.target = 1; return; }
    enemy_wander(e);
}

void CombatWorld::enemy_walk_boundary(std::size_t index, HeroCombatView& hero) { // al.h
    auto& e = enemies_[index];
    if (e.wander < e.move_delay * 2 && next_int() > 0) {
        e.wander += e.move_delay;
        e.move_cooldown = 0;
        if (e.boss != BossKind::none) boss_think(index, hero); else enemy_think(index, hero);
        return;
    }
    enemy_rest(e, false);
    if (e.boss != BossKind::none) boss_think(index, hero); else enemy_think(index, hero);
}

void CombatWorld::enemy_attack_frame(std::size_t index, HeroCombatView& hero) { // al.j
    auto& e = enemies_[index];
    const auto& s = stat(e);
    e.hidden = false;
    const int hit_frame = (e.sprite >= 0 && std::size_t(e.sprite) < kEnemyHitFrame.size()) ? kEnemyHitFrame[std::size_t(e.sprite)] - 1 : 0;
    if (e.frame != hit_frame) return;
    if (s.attack_kind <= 1) {
        if (melee_reaches_hero(index)) hit_hero(index, s.attack, e.heading(), hero);
        return;
    }
    for (int d = 1; d <= 3; ++d) {
        if (occupant_at(e, e.heading(), d) != -1) continue;
        if (s.attack_kind == 2) { // al.j: aw(9,-1,n) on the hero, then ao.a(al,byte)
            eif_effect(e.sprite, -1);
            hit_hero(index, s.attack, e.heading(), hero);
            return;
        }
        Projectile p; // new i(x+u[j], y+u[j], ce.f[n], this, j, 3, 2): facing, not o.i
        p.x = e.tile_x + kDx[std::size_t(e.direction)]; p.y = e.tile_y + kDy[std::size_t(e.direction)];
        p.direction = e.direction; p.range = 2; p.step_frame = 2; p.source = int(index);
        p.source_generation = e.generation; p.source_stats = e.stats;
        p.damage = s.attack; p.effect = e.sprite;
        p.duration = frames_[std::size_t(std::clamp(e.sprite, 0, int(frames_.size()) - 1))].effect;
        if (in_bounds(p.x, p.y)) projectiles_.push_back(p);
        return;
    }
}

void CombatWorld::hit_hero(std::size_t enemy_index, int damage, int direction, HeroCombatView& hero, int gone_stats) {
    // ao.a(al,short,byte)
    if (hero_dying_ || hero.hp <= 0) return;
    if (status_.invulnerable) return; // p (guardian 4, skill 1)
    const bool gone = gone_stats >= 0 && stats_ && std::size_t(gone_stats) < stats_->size();
    if (!gone) select_target(enemy_index,true);
    auto& e = enemies_[enemy_index];
    const auto& s = gone ? (*stats_)[std::size_t(gone_stats)] : stat(e);
    // A departed shooter is no longer in the world: its reflected damage reaches nobody visible.
    if (!gone && status_.thorns && e.state != EnemyState::dying && e.state != EnemyState::dead) { // ao.g: reflect
        e.hp -= hero.guardian_level * 2 + 40 + hero.base_will;
        add_popup(PopupKind::damage, hero.guardian_level * 2 + 40 + hero.base_will, e.x_px + 8, e.y_px + 8 + e.draw_offset_y, int(enemy_index));
        if (e.hp <= 0) {
            e.state = EnemyState::dying; e.frame = -1; e.death_timer = 3;
            if (e.boss != BossKind::none) boss_death(enemy_index);
        }
    }
    int evade = hero.agility - s.evasion + 10 + hero.accessory_evasion;
    evade = std::clamp(evade, 8, 60);
    const int hx = hero.x_tile * 16 + 8, hy = hero.y_tile * 16 + 8;
    if (rand_range(0, 99) < evade) { add_popup(PopupKind::miss, 0, hx, hy, -1); return; }
    int amount = damage + rand_range(-(damage / 10), damage / 10) - hero.defense * (status_.defense_up ? 2 : 1);
    if (amount > 0) {
        hero_damaged(hero, amount);
        hit_effect(6,-1,hero.x_tile*16+8,hero.y_tile*16+8);
    }
    if (amount < 0) amount = 0;
    add_popup(PopupKind::damage, -amount, hx, hy, -1);
    if (s.attack_kind == 1 && rand_range(0, 99) < 15) {
        if(status_.poison==0) status_.order.push_back(7);
        status_.poison = 80; status_.poison_tick = 0;
    } // cf 7
    hero_shake_=1;hero_shake_direction_=std::clamp(direction,1,4);
}

void CombatWorld::hero_damaged(HeroCombatView& hero, int amount) { // ao.b(-amount)
    hero.hp = std::clamp(hero.hp - amount, 0, hero.max_hp);
    if (hero.hp == 0 && !hero_dying_) {
        hero_dying_ = true; hero_state_ = 6; hero_frame_ = 0; death_timer_ = 24;
    }
}

void CombatWorld::hit_enemy(std::size_t index, int damage, bool knock, int direction, bool critical,
                            int knock_power, int status, HeroCombatView& hero,int hit_kind) {
#if defined(HEROES_LORE_PSP_BUILD)
    psp_diag_stage("hit-enemy-begin",int(index),damage);
#endif
    // al.a(int,boolean,byte,boolean,byte,byte,ao)
    auto& e = enemies_[index];
    if (e.state == EnemyState::dying || e.state == EnemyState::dead) return;
    select_target(index);
    if (e.boss != BossKind::none) { hit_boss(index, damage, critical, hero,hit_kind,status); return; }
    const auto& s = stat(e);
    const int attack_element = hero_attack_element(hero);
    if (!e.provoked && s.summoner && attack_element != s.summon_element) e.summon_timer = 40;
    e.provoked = true;
    e.hidden = false;
    e.status_elapsed[0]=-1; // al.t wakes Sleep even when this attack later misses.
    e.status_order.erase(std::remove(e.status_order.begin(),e.status_order.end(),0),e.status_order.end());
    if (s.half_damage) damage /= 2;
    damage -= e.status_elapsed[4]>=0?s.defense/2:s.defense;
    if (damage < 0) damage = 0;
    damage = damage * kElement[std::size_t(attack_element)][std::size_t(s.element)] / 10;
    if(critical) damage+=damage*hero.weapon_critical_bonus_tenths/10;
    int evade = s.evasion - hero.agility - hero.weapon_refine / 5 + 10;
    if (evade > 50) evade = 50;
    const int ex = e.x_px + 8 + (e.width - 1) * 8, ey = e.y_px + 8;
    if (rand_range(0, 99) < evade) { add_popup(PopupKind::miss, 0, ex, ey, int(index)); sounds_.push_back(14); return; }
    sounds_.push_back(critical ? 15 : 13); // al.a: bw 15 critical, 13 normal hit
    if(status>=0 && status<int(kOptionStatus.size())) {
        const int effect=kOptionStatus[status];
        if(effect>=0) {
            if(e.status_elapsed[effect]<0) e.status_order.push_back(effect);
            e.status_elapsed[effect]=0;
        }
        if(status==2) damage=s.hp;
        else if(status==3) hero.sp=std::clamp(hero.sp+damage*80/100,0,hero.max_sp);
        else if(status==4) hero.hp=std::clamp(hero.hp+damage/2,0,hero.max_hp);
        else if(status==8) damage*=2;
    }
    if (knock && e.hp > 0 && !e.partial_x && !e.partial_y) { e.state = EnemyState::knockback; e.knockback = 2; e.knock_direction = direction; } // o.i only
    (void)knock_power;
    e.shake = 4; e.shake_direction = direction;
    e.hp -= damage; // al.b(int)
    hit_effect(hit_kind,int(index),ex,ey);
    add_popup(PopupKind::damage, damage, ex, ey, int(index));
    if (e.hp <= 0) { e.state = EnemyState::dying; e.frame = -1; e.death_timer = 3; }
#if defined(HEROES_LORE_PSP_BUILD)
    psp_diag_stage("hit-enemy-done",int(index),e.hp);
#endif
}

void CombatWorld::enemy_die(std::size_t index, HeroCombatView& hero, CombatEvents& events) { // al.l
#if defined(HEROES_LORE_PSP_BUILD)
    psp_diag_stage("enemy-death-begin",int(index));
#endif
    auto& e = enemies_[index];
    const auto& s = stat(e);
    battle_effects_.push_back({100+std::clamp(s.size,0,2),-2,e.tile_x*16+8,e.tile_y*16+9,0,death_effect_frames_[std::clamp(s.size,0,2)]});
    if(target_==int(index)) {target_=-1;target_ticks_=0;}
    unregister(int(index));
    e.state = EnemyState::dead;
    if (s.respawn != 2) spawns_.push_back({s.respawn_delay, e.sprite, e.stats, e.origin_x, e.origin_y});
    int roll = rand_range(1, 150);
    int type = -1, id = -1;
    for (const auto& drop : s.drops) {
        roll -= drop.chance;
        if (roll > 0) continue;
        if (drop.chance == 1) {
            if (rand_range(1, 100) > 20) continue;
        }
        type = drop.type; id = drop.id;
        break;
    }
    if (type != -1 && type != 22) drops_.push_back({e.tile_x, e.tile_y, type, id, 1, 120});
    if (rand_range(1, 100) <= 60) {
        const int gold = s.level * 3;
        drops_.push_back({e.tile_x, e.tile_y, -1, gold / 100, gold % 100, 120});
    }
    if (rand_range(1, 100) <= 20 + (s.level - hero.level)) drops_.push_back({e.tile_x, e.tile_y, 11, 0, 1, 120});
    int factor = 20 - (hero.level - s.level);
    if (factor > 26) factor = 26;
    const int exp = s.level * factor / 2;
    if (exp > 0) events.experience += exp;
#if defined(HEROES_LORE_PSP_BUILD)
    psp_diag_stage("enemy-death-done",int(index),int(drops_.size()));
#endif
}

void CombatWorld::update_enemy(std::size_t index, HeroCombatView& hero, CombatEvents& events) {
    auto& e = enemies_[index];
    if (e.state == EnemyState::dead) return;
    if (e.boss != BossKind::none) { update_boss(index, hero, events); return; }
    const auto& s = stat(e);
    const auto& fr = frames_[std::size_t(std::clamp(e.sprite, 0, int(frames_.size()) - 1))];
    if (has_visible_) { // al.a(Graphics,II): f from the draw anchor with inclusive margins
        const int ax = e.x_px + 8 + (e.width - 1) * 8, ay = e.y_px + 8;
        e.on_screen = ax + 16 >= visible_[0] && ax - 16 <= visible_[2] &&
                      ay >= visible_[1] && ay <= visible_[3] + 32;
    }
    ++e.frame; // al.d
    // al.s runs before the offscreen early-out; Bleeding can be lethal.
    for(int status=0;status<5;++status) if(e.status_elapsed[status]>=0) {
        const int elapsed=++e.status_elapsed[status];
        if(status==3 && elapsed%8==0) {
            const int damage=rand_range(15,25);e.hp-=damage;
            add_popup(PopupKind::damage,damage,e.x_px+8+(e.width-1)*8,e.y_px+8+e.draw_offset_y,int(index));
            if(e.hp<=0) {e.state=EnemyState::dying;e.frame=-1;e.death_timer=3;} // al.b resets even an existing death animation
        }
        if(elapsed>=40) e.status_elapsed[status]=-1;
    }
    e.status_order.erase(std::remove_if(e.status_order.begin(),e.status_order.end(),[&](int id){return e.status_elapsed[id]<0;}),e.status_order.end());
    const int ax = std::abs(hero.x_tile - e.tile_x), ay = std::abs(hero.y_tile - e.tile_y);
    if (!e.on_screen && (ax > s.aggro_range || ay > s.aggro_range) && e.target == 0) return;
    if (e.shake > 0) --e.shake;
    // al.n
    const bool aligned = !e.partial_x && !e.partial_y;
    // al.o: a provoked summoner's timer runs in every state, the death ticks included.
    const auto summon_due = [&](Enemy& actor) {
        if (!s.summoner || !actor.provoked) return false;
        if (actor.summon_timer > 0) --actor.summon_timer;
        if (actor.summon_timer != 0) return false;
        actor.summon_timer = -10;
        return true;
    };
    if (e.state == EnemyState::dying) {
        const bool finish = e.death_timer < 1;
        if (!finish) --e.death_timer;
        // The call precedes ae.m dropping the corpse, so the corpse still blocks its own tile.
        if (summon_due(e)) spawn_enemy(e.tile_x, e.tile_y, e.sprite, e.stats, true, 1, 5);
        if (finish) enemy_die(index, hero, events); // al.l
        return;
    }
    if(e.status_elapsed[0]>=0 || e.status_elapsed[2]>=0) enemy_rest(e,false);
    else switch (e.state) {
        case EnemyState::idle: enemy_think(index, hero); break;
        case EnemyState::walk: if (aligned) enemy_walk_boundary(index, hero); break;
        case EnemyState::attack: if (e.frame >= fr.attack) { enemy_rest(e, false); enemy_think(index, hero); } break;
        case EnemyState::knockback: if (e.knockback < 1) e.state = EnemyState::idle; --e.knockback; break; // al.a(byte) keeps o.k
        default: break;
    }
    // o.e: walking / knocked-back actors step 8 px per tick.
    if (e.state == EnemyState::walk || e.state == EnemyState::knockback) {
        const int self = int(index) + 1;
        if (!e.partial_x && !e.partial_y && !can_step(self, e.heading())) e.state = EnemyState::idle; // o.a(): a(1) keeps o.k
        else move_enemy(e, int(index), 8);
    }
    // al.o. Append only after the last use of e: vector growth invalidates it.
    const bool summon = summon_due(e);
    switch (e.state) {
        case EnemyState::walk: if (e.frame >= fr.walk) e.frame = 0; break;
        case EnemyState::attack: enemy_attack_frame(index, hero); break;
        default:
            if (e.frame >= fr.idle) e.frame = 0;
            if (e.attack_cooldown > 0) --e.attack_cooldown;
            if (e.move_cooldown > 0) --e.move_cooldown;
            break;
    }
    if (summon) spawn_enemy(e.tile_x, e.tile_y, e.sprite, e.stats, true, 1, 5);
}

void CombatWorld::update_projectiles(HeroCombatView& hero) { // i.a + y lifetime
    const std::size_t count = projectiles_.size();
    for (std::size_t i = 0; i < count; ++i) {
        auto p = projectiles_[i];
        if (p.frame == p.step_frame && p.range > 0 && (p.pierce || !p.spent)) {
            Projectile next = p;
            next.x += kDx[std::size_t(p.direction)]; next.y += kDy[std::size_t(p.direction)];
            next.range = p.range - 1; next.frame = 0; next.spent = false;
            if (in_bounds(next.x, next.y)) projectiles_.push_back(next);
        }
        if ((p.pierce || !p.spent) && p.frame == 1) {
            const int who = occupant(p.x, p.y);
            if (!p.from_hero && who == -1 && p.source >= 0 && std::size_t(p.source) < enemies_.size()) {
                const bool same = enemies_[std::size_t(p.source)].generation == p.source_generation;
                hit_hero(std::size_t(p.source), p.damage, p.direction, hero, same ? -1 : p.source_stats);
                p.spent = true;
            } else if (p.from_hero && who > 0) {
                hit_enemy(std::size_t(who - 1), p.damage, false, p.direction, p.critical, 1, p.status, hero);
                p.spent = true;
            }
        }
        // ae.m: i.a()Z is checked on the frame just processed; only a surviving i is painted,
        // and painting (i.a(Graphics,II)) advances b. A two-frame arrow therefore still
        // propagates on frame 2 before it ends.
        const bool done = p.frame >= p.duration || (p.duration == 2 && !p.pierce && p.spent && p.frame >= 1);
        if (done) p.duration = -1; else ++p.frame;
        projectiles_[i] = p;
    }
    projectiles_.erase(std::remove_if(projectiles_.begin(), projectiles_.end(), [](const Projectile& p) {
        return p.duration < 0;
    }), projectiles_.end());
}

int CombatWorld::hero_sprite_state() const { // ao.e
    if (hero_dying_) return 2;
    if (hero_state_ != 3) return -1;
    const int q = std::max(combo_index_, 0); // before the first tick q is still -1
    const int type = combo_[std::size_t(q)];
    switch (q) {
        case 0: return type == 1 ? 3 : 7;
        case 1: return 4;
        case 2: return type == 1 ? 5 : 8;
        case 3: return type == 1 ? 6 : 9;
        case 4: return 10;
        default: return -1;
    }
}

bool CombatWorld::request_attack(const HeroCombatView& hero, bool skill, int* face) { // n.a(boolean) + ao.a(boolean)
    if (!hero.combat_map || hero_dying_ || hero.hp <= 0) return false;
    max_combo_ = std::clamp(hero.max_combo, 1, 5);
    const bool attacking = hero_state_ == 3;
    if (combo_index_ + 1 >= max_combo_) return false;
    if (recover_ > 0) return false;
    if (combo_[std::size_t(combo_index_ + 1)] == 0) {
        if (combo_index_ >= 0 && combo_[std::size_t(combo_index_)] == 2) return false;
        if (combo_index_ == 0 && skill) return false;
        if (combo_index_ == 3 && !skill) return false;
        combo_[std::size_t(combo_index_ + 1)] = skill ? 2 : 1;
    }
    if (!attacking) {
        // ao.a(3) + ao.i(): the step index stays -1 until the next tick; turn toward a
        // flanking or rear enemy when nothing stands in front.
        hero_state_ = 3; hero_frame_ = 0; dash_ = 0;
        const int dir = hero.direction;
        if (face && dir >= 1 && dir <= 4 && !enemy_in_front(hero, dir, 1)) {
            for (int candidate : {kSideF[std::size_t(dir)], kSideE[std::size_t(dir)], kBack[std::size_t(dir)]})
                if (enemy_in_front(hero, candidate, 1)) { *face = candidate; break; }
        }
    }
    return true;
}

void CombatWorld::end_combo(int index) { // ao.h(int)
    if (index == -1 || (index == 0 && combo_[0] == 1)) recover_ = 1;
    else if (index == 0 && combo_[0] == 2) recover_ = 3;
    else recover_ = combo_[std::size_t(index)] == 1 ? 1 : 3;
    combo_.fill(0);
    combo_index_ = -1;
    hero_state_ = 1;
    hero_frame_ = hero_render_frame_ = 0;
    dash_ = 0;
}

int CombatWorld::enemy_in_front(const HeroCombatView& hero, int direction, int distance) const {
    const int who = occupant(hero.x_tile + kDx[std::size_t(direction)] * distance,
                             hero.y_tile + kDy[std::size_t(direction)] * distance);
    return who > 0 ? who : 0;
}

void CombatWorld::hero_strike(int target, int knock, int direction, int power, HeroCombatView& hero) {
    (void)power;
    int kind=1;
    if(hero.character_class==6 && combo_index_>=0 &&
       ((combo_[combo_index_]==1 && combo_index_==3) || (combo_[combo_index_]==2 && combo_index_==4))) kind=5;
    if (target > 0) hit_enemy(std::size_t(target - 1), strike_damage_, knock != 0, direction, strike_critical_, knock, strike_status_, hero,kind);
}

bool CombatWorld::hero_class6_hit(HeroCombatView& hero) { // ao.b: sword
    const int type = combo_[std::size_t(combo_index_)];
    const int q = combo_index_, dir = hero.direction;
    const int knock = ((type == 1 && q == 3) || (type == 2 && q == 4)) ? 5 : 1;
    bool hit = false;
    if ((type == 1 && (q == 0 || q == 3)) || (type == 2 && q == 4 && hero_frame_ == 6)) {
        if (int t = enemy_in_front(hero, dir, 1)) { hero_strike(t, 0, dir, knock, hero); hit = true; }
    } else if ((type == 1 && (q == 1 || q == 2)) || (type == 2 && q == 0)) {
        int last = 0;
        for (int d : {dir, kDiagD[std::size_t(dir)], kDiagC[std::size_t(dir)]}) {
            const int t = enemy_in_front(hero, d, 1);
            if (t && t != last) { hero_strike(t, 0, dir, knock, hero); hit = true; }
            last = t;
        }
    } else if (type == 2 && q == 4) {
        for (int d = 1; d <= 8; ++d)
            if (int t = enemy_in_front(hero, d, 1)) { hero_strike(t, 1, d, knock, hero); hit = true; }
    } else if (type == 2 && (q == 2 || q == 3)) {
        if (int t = enemy_in_front(hero, dir, 1)) { hero_strike(t, 0, dir, knock, hero); hit = true; }
        const int dx = kDx[std::size_t(dir)], dy = kDy[std::size_t(dir)];
        const auto overlay_at = [&](int x, int y) { return in_bounds(x, y) ? overlay_[std::size_t(y) * std::size_t(width_) + std::size_t(x)] : -1; };
        if (overlay_at(hero.x_tile + dx, hero.y_tile + dy) == 0 && tile_free(hero.x_tile + 2 * dx, hero.y_tile + 2 * dy)) dash_ = 2;
        else if (tile_free(hero.x_tile + dx, hero.y_tile + dy)) dash_ = 1;
        if (q == 3 && dash_) {
            if (int t = enemy_in_front(hero, dir, 1 + dash_)) { hero_strike(t, 0, dir, knock, hero); hit = true; }
        }
    }
    return hit;
}

bool CombatWorld::hero_class7_hit(HeroCombatView& hero) { // ao.c: spear
    const int type = combo_[std::size_t(combo_index_)];
    const int dir = hero.direction;
    bool hit = false;
    if (type == 1 && combo_index_ == 3) {
        int last = 0;
        for (int d : {dir, kDiagD[std::size_t(dir)], kDiagC[std::size_t(dir)]}) {
            const int t = enemy_in_front(hero, d, 1);
            if (t && t != last) { hero_strike(t, 0, dir, 1, hero); hit = true; }
            last = t;
        }
        return hit;
    }
    for (int distance : {1, 2})
        if (int t = enemy_in_front(hero, dir, distance)) { hero_strike(t, 0, dir, 1, hero); hit = true; }
    return hit;
}

bool CombatWorld::hero_class8_hit(HeroCombatView& hero) { // ao.d: magic
    const int type = combo_[std::size_t(combo_index_)];
    const int dir = hero.direction;
    if (type == 2 && combo_index_ >= 2 && combo_index_ <= 4) {
        Projectile p; // new i(..., ce.b[q-2], this, true, dir, 3, 2, damage, status, crit)
        p.x = hero.x_tile + kDx[std::size_t(dir)]; p.y = hero.y_tile + kDy[std::size_t(dir)];
        p.direction = dir; p.range = 2; p.step_frame = 2; p.from_hero = true; p.pierce = true;
        p.damage = strike_damage_; p.status = strike_status_; p.critical = strike_critical_;
        p.effect = combo_index_ - 2;
        p.duration = std::max(1, hero.magic_frames[std::size_t(p.effect)]); // y(BB[B): a = EIF frame count
        if (in_bounds(p.x, p.y)) projectiles_.push_back(p);
        return false;
    }
    if (int t = enemy_in_front(hero, dir, 1)) { hero_strike(t, 0, dir, 1, hero); return true; }
    return false;
}

int CombatWorld::roll_hit_option(const HeroCombatView& hero) {
    for(int option:{hero.weapon_hit_option,hero.shield_hit_option})
        if(option>=0 && option<int(kOptionChance.size()) && rand_range(0,99)<kOptionChance[option]) return option;
    return -1;
}

void CombatWorld::hero_hit_frame(HeroCombatView& hero) { // ao.p
    // ao.a(): attack with combo multiplier and random spread.
    int v = hero.attack;
    if (status_.attack_up) v = v * 3 / 2; // ao.d:Z from guardian 0 skill 1
    if (combo_[std::size_t(combo_index_)] == 2) v = v * 17 / 10;
    else {
        static constexpr int scale[5] = {10, 12, 13, 14, 17};
        v = v * scale[combo_index_] / 10;
    }
    if (v >= 10) v += next_int() % (v / 10);
    strike_damage_ = v;
    strike_status_ = roll_hit_option(hero);
    strike_critical_ = std::abs(next_int() % 100) < hero_critical_chance(hero);
    // ao.p charges Rage once per strike, only with an enemy directly ahead,
    // before evasion and class-specific multi-target attacks.
    if(enemy_in_front(hero,hero.direction,1)>0) {
        const int icon=strike_status_==8?0:strike_status_==2?8:strike_status_==3?10:strike_status_==4?11:-1;
        if(icon>=0) battle_effects_.push_back({200+icon,-1,0,0,0,8});
    }
    if(strike_status_==8 && enemy_in_front(hero,hero.direction,1)>0) {
        const int cost=hero.max_hp/25;
        hero_damaged(hero,cost);
        add_popup(PopupKind::damage,-cost,hero.x_tile*16+8,hero.y_tile*16+8,-1);
    }
    const bool hit = hero.character_class == 6 ? hero_class6_hit(hero)
                   : hero.character_class == 7 ? hero_class7_hit(hero) : hero_class8_hit(hero);
    if (!hit) sounds_.push_back(14); // ao.p: swing sound when nothing was hit
}

void CombatWorld::update_hero_attack(HeroCombatView& hero, CombatEvents& events) { // ao.d case 3 -> ao.o
    (void)events;
    if (hero_state_ != 3) return;
    if (combo_index_ == -1) combo_index_ = 0;
    const int cls = std::clamp(hero.character_class - 6, 0, 2);
    const int type = combo_[std::size_t(combo_index_)];
    if (type < 1 || type > 2) { end_combo(combo_index_); return; }
    if (hero_frame_ == kComboFrames[cls][type - 1][combo_index_]) {
        if (combo_index_ + 1 == max_combo_ || combo_[std::size_t(combo_index_ + 1)] == 0) { end_combo(combo_index_); return; }
        ++combo_index_;
        hero_frame_ = 0;
    }
    dash_ = 0;
    const int step_type = combo_[std::size_t(combo_index_)];
    if (hero_frame_ == 0) {
        int cost = hero.weapon_power / 4 + 4;
        if (step_type == 2) cost = cost * 7 / 5;
        if (hero.sp < cost && !(combo_index_ == 0 && step_type == 1)) { end_combo(combo_index_ - 1); return; }
        hero.sp = std::clamp(hero.sp - cost, 0, hero.max_sp);
    }
    const int k = hero_frame_;
    bool strike = false;
    switch (hero.character_class) {
        case 6: strike = (step_type == 2 && combo_index_ == 4) ? (k == 1 || k == 6) : k == 2; break;
        case 7:
            if (step_type == 2 && combo_index_ == 4) strike = k % 2 == 0 && k <= 10;
            else if (step_type == 2 && combo_index_ == 3) strike = k == 4;
            else strike = k == 1;
            break;
        default: strike = (step_type == 2 && combo_index_ == 4) ? k == 4 : k == 1; break;
    }
    if (strike) hero_hit_frame(hero);
    hero_render_frame_=hero_frame_;
    ++hero_frame_;
}

void CombatWorld::age_attached() {
    for(auto& effect:battle_effects_) ++effect.frame;
    battle_effects_.erase(std::remove_if(battle_effects_.begin(),battle_effects_.end(),[](const BattleEffect& e){return e.frame>=e.duration;}),battle_effects_.end());
    for(auto& popup:popups_) ++popup.age;
    popups_.erase(std::remove_if(popups_.begin(),popups_.end(),[](const Popup& p){return p.age>=p.duration;}),popups_.end());
}

void CombatWorld::tick(HeroCombatView& hero, CombatEvents& events) {
    events.shake_x=events.shake_y=0;
    if(target_ticks_>0) --target_ticks_;
    if(hero_shake_>0) --hero_shake_;
    age_attached();
    hero_x_ = hero.x_tile; hero_y_ = hero.y_tile;
    rebuild_occupancy(hero);
    place_initial_spawns(hero);
    if (recover_ > 0) --recover_;
    max_combo_ = std::clamp(hero.max_combo, 1, 5);
    // ao.d field regeneration: idle ticks count double, attacks pause the timers.
    if (!hero_dying_ && hero_state_ != 3) {
        hp_regen_timer_ -= hero.moving ? 1 : 2;
        if (hp_regen_timer_ <= 0) {
            hero.hp = std::clamp(hero.hp + hero.hp_attribute * (status_.regen_double ? 2 : 1), 0, hero.max_hp);
            hp_regen_timer_ = std::min(67 + hero.level, 100);
        }
        sp_regen_timer_ -= hero.moving ? 1 : 3;
        if (sp_regen_timer_ <= 0) {
            hero.sp = std::clamp(hero.sp + hero.will * (status_.regen_double ? 2 : 1), 0, hero.max_sp);
            sp_regen_timer_ = 21;
        }
    }
    if (hero_dying_) {
        hero_frame_ = 0;
        if (death_timer_ > 0) --death_timer_;
        else events.hero_died = true;
    } else {
        // ao.d: the cf loop (expiry clears ao.d/ao.e) runs before the attack step ao.o.
        update_status(hero);
        update_hero_attack(hero, events);
    }
    // n.n calls p.e after ao.d without checking the hero: the guardian and the world
    // effects (ae objects) keep running through the death animation.
    age_effects();
    update_guardian(hero, events);
    update_spawns();
    for (auto& d : drops_) --d.ttl;
    drops_.erase(std::remove_if(drops_.begin(), drops_.end(), [](const GroundDrop& d) { return d.ttl < 0; }), drops_.end());
    for (std::size_t i = 0; i < enemies_.size(); ++i) update_enemy(i, hero, events);
    update_projectiles(hero);
    shake_x_ = events.shake_x;shake_y_ = events.shake_y;

}

bool CombatWorld::try_pickup(const HeroCombatView& hero, CombatEvents& events,
                             const std::function<bool(int,int)>& bag_has_room) {
    // ae.a(BB)[B: an item that does not fit stays on the ground and the search goes on.
    bool refused = false;
    for (std::size_t i = 0; i < drops_.size(); ++i) {
        const auto& d = drops_[i];
        if (d.x != hero.x_tile || d.y != hero.y_tile) continue;
        if (d.type == -1) {
            int amount = d.gold();
            if (amount > 0) amount *= 9;
            events.gold += amount;
        } else {
            if (!bag_has_room(d.type, d.id)) { refused = true; continue; }
            events.items.emplace_back(d.type, d.id);
        }
        drops_.erase(drops_.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
    }
    if (refused) events.messages.push_back("bag_full");
    return false;
}

void CombatWorld::add_popup(PopupKind kind, int value, int x_px, int y_px, int target) {
    const int generation = target >= 0 && std::size_t(target) < enemies_.size() ? enemies_[std::size_t(target)].generation : 0;
    popups_.push_back({kind, value, x_px, y_px, 0, 4, target, generation}); // aw.h[2] and aw(7,4,..) both last 4 frames
}

} // namespace combat
