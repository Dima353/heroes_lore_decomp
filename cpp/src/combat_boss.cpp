// Boss encounters: av (base) and its subclasses cc, ar, ag, bd, cd, ba, ak, cg, bv,
// created by the ae constructor for maps 11, 13, 15 and 82.
#include "combat.hpp"

#include <algorithm>
#include <cstdlib>

namespace combat {
namespace {

// cc.h / cc.i: attack set rotation and the cooldowns that follow each set.
constexpr int kCcPattern[6] = {1, 1, 2, 3, 2, 3};
constexpr int kCcDelay[6] = {2, 2, 12, 24, 12, 24};

} // namespace

int CombatWorld::add_boss(BossKind kind, int x, int y, int stats_index, int width, int state, int direction) {
    if (!stats_ || stats_index < 0 || std::size_t(stats_index) >= stats_->size()) return -1;
    const auto& s = (*stats_)[std::size_t(stats_index)];
    Enemy e;
    e.boss = kind;
    e.sprite = stats_index; e.stats = stats_index;
    e.x_px = x * 16; e.y_px = y * 16; e.tile_x = x; e.tile_y = y;
    e.origin_x = x; e.origin_y = y;
    e.width = width; e.hp = s.hp;
    e.move_delay = s.move_delay; e.move_cooldown = s.move_delay; e.attack_cooldown = s.attack_delay;
    e.state = static_cast<EnemyState>(state); e.direction = direction;
    e.provoked = true; // bosses always know where the hero is (av.d)
    e.generation = ++next_generation_;
    enemies_.push_back(e);
    const int index = int(enemies_.size()) - 1;
    register_actor(index);
    return index;
}

void CombatWorld::spawn_bosses(int map_id) {
    switch (map_id) {
        case 11: { // ae.e/f
            const int i = add_boss(BossKind::cc, 10, 9, 32, 1, 1, 2);
            if (i >= 0) cc_set_pattern(enemies_[std::size_t(i)], kCcPattern[0]);
            break;
        }
        case 13: // ae.g + ae.a(true); the second phase comes from ar's death
            add_boss(BossKind::ar, 9, 5, 35, 1, 1, 2);
            enemies_.back().flying = true;
            break;
        case 15: { // ae.h/i: hands sit 5/4 tiles below their drawn position and never collide
            const int ba = add_boss(BossKind::ba, 0, 12, 40, 2, 1, 1);
            const int ak = add_boss(BossKind::ak, 13, 11, 41, 1, 1, 1);
            for (int hand : {ba, ak}) if (hand >= 0) {
                unregister(hand);
                auto& h = enemies_[std::size_t(hand)];
                h.ghost = true; h.move_delay = 2; h.draw_direction = 1;
            }
            if (ba >= 0) enemies_[std::size_t(ba)].draw_offset_y = -80;
            if (ak >= 0) { enemies_[std::size_t(ak)].draw_offset_y = -64; enemies_[std::size_t(ak)].pattern = 2; }
            const int cg = add_boss(BossKind::cg, 7, 7, 39, 2, 2, 1);
            if (cg >= 0) { enemies_[std::size_t(cg)].partner_a = ba; enemies_[std::size_t(cg)].partner_b = ak; }
            break;
        }
        case 82: // ae.j/k
            add_boss(BossKind::bv, 7, 10, 42, 1, 1, 2);
            break;
        default: break;
    }
}

bool CombatWorld::boss_encounter() const {
    return std::any_of(enemies_.begin(), enemies_.end(), [](const Enemy& e) {
        return e.boss != BossKind::none && e.state != EnemyState::dead;
    });
}

void CombatWorld::cc_set_pattern(Enemy& e, int pattern) { // cc.d(byte)
    e.pattern = pattern;
    const auto& fr = frames_[std::size_t(std::clamp(e.sprite, 0, int(frames_.size()) - 1))];
    e.attack_length = fr.alt_attack[std::size_t(std::clamp(pattern - 1, 0, 2))];
}

void CombatWorld::remove_enemy(std::size_t index) { // av.l -> ae.a(ck)
    unregister(int(index));
    enemies_[index].state = EnemyState::dead;
    enemies_[index].body_row = -1;
}

bool CombatWorld::in_area(const HeroCombatView& hero, int x0, int x1, int y0, int y1) const {
    return hero.x_tile >= x0 && hero.x_tile <= x1 && hero.y_tile >= y0 && hero.y_tile <= y1;
}

void CombatWorld::boss_projectile(std::size_t index, int x, int y, int direction, int range) {
    if (!in_bounds(x, y)) return;
    Projectile p; // new i(x, y, ce.f[n], this, dir, range, 2)
    p.x = x; p.y = y; p.direction = direction; p.range = range - 1; p.step_frame = 2;
    p.source = int(index); p.damage = stat(enemies_[index]).attack; p.effect = enemies_[index].sprite;
    p.source_generation = enemies_[index].generation; p.source_stats = enemies_[index].stats;
    p.duration = frames_[std::size_t(std::clamp(p.effect, 0, int(frames_.size()) - 1))].effect;
    projectiles_.push_back(p);
}

void CombatWorld::eif_effect(int sprite, int target, int x_px, int y_px) {
    if (sprite < 0) return;
    const int frames = frames_[std::size_t(std::clamp(sprite, 0, int(frames_.size()) - 1))].effect;
    battle_effects_.push_back({kEifEffectBase + sprite, target, x_px, y_px, 0, std::max(1, frames)});
}

void CombatWorld::heal_enemy(std::size_t source, std::size_t target) { // cd.a(al)
    auto& t = enemies_[target];
    if (t.state == EnemyState::dead || t.state == EnemyState::dying) return;
    const auto& src = enemies_[source];
    eif_effect(src.sprite, int(target)); // new aw(9,-1,n) on the healed body
    const std::size_t body = src.partner_a >= 0 ? std::size_t(src.partner_a) : source;
    const int amount = stat(enemies_[body]).hp / 10;
    t.hp = std::min(t.hp + amount, stat(t).hp); // al.c(int)
    add_popup(PopupKind::damage, -amount, t.x_px + 8 + (t.width - 1) * 8, t.y_px + 8, int(target));
}

void CombatWorld::cc_teleport_near(std::size_t index, HeroCombatView& hero) { // cc.o, walk frame 5
    auto& e = enemies_[index];
    int x = -1, y = -1, tries = 3;
    while (tries > 0 && !tile_free(x, y)) {
        x = hero.x_tile > e.tile_x ? rand_range(e.tile_x, hero.x_tile + 1) : rand_range(hero.x_tile - 1, e.tile_x);
        y = hero.y_tile > e.tile_y ? rand_range(e.tile_y, hero.y_tile + 1) : rand_range(hero.y_tile - 1, e.tile_y);
        --tries;
    }
    if (tries > 0) {
        unregister(int(index));
        e.x_px = x * 16; e.y_px = y * 16; e.tile_x = x; e.tile_y = y; e.partial_x = e.partial_y = false;
        register_actor(int(index));
    }
}

void CombatWorld::boss_think(std::size_t index, HeroCombatView& hero) {
    auto& e = enemies_[index];
    const int f = std::abs(hero.x_tile - e.tile_x), g = std::abs(hero.y_tile - e.tile_y);
    switch (e.boss) {
        case BossKind::cc: // cc.i
            if (e.attack_cooldown == 0) {
                const bool start = e.pattern == 1 ? (f < 4 && g < 4)
                                 : e.pattern == 2 ? (f * g == 0 && f < 4 && g < 4) : e.pattern == 3;
                if (start) { enemy_start_attack(e); return; }
            }
            if (e.move_cooldown == 0) {
                const bool walk = e.pattern == 1 ? (f >= 4 || g >= 4)
                                : e.pattern == 2 ? (f * g != 0 || f >= 4 || g >= 4) : (f >= 3 || g >= 3);
                if (walk) { e.state = EnemyState::walk; e.frame = 0; }
            }
            return;
        case BossKind::ar: // ar.i
            if (e.attack_cooldown == 0 && f <= 1) { e.direction = 2; enemy_start_attack(e); return; }
            if (e.move_cooldown == 0) {
                // a(2)/a(1) keep o.k (al.a(byte)); only q() and a(boolean) rewind it.
                if (f > 1) { e.state = EnemyState::walk; e.direction = hero.x_tile > e.tile_x ? 4 : 3; }
                else { e.state = EnemyState::idle; e.direction = 2; }
            }
            return;
        case BossKind::ag:
            if (e.attack_cooldown == 0) { e.direction = 2; enemy_start_attack(e); }
            return;
        case BossKind::bd: case BossKind::cd:
            if (e.attack_cooldown == 0) enemy_start_attack(e);
            return;
        case BossKind::cg: // cg.i: three tongue attacks, then a 40-tick pause
            if (e.counter >= 3) { e.attack_cooldown = 40; e.counter = 0; }
            if (e.attack_cooldown == 0) { e.direction = 2; enemy_start_attack(e); ++e.counter; }
            return;
        case BossKind::ba: case BossKind::ak: {
            const bool left = e.boss == BossKind::ba;
            const int base = e.tile_y - (left ? 5 : 4) + (left ? 3 : 2);
            const int dy = hero.y_tile - base;
            const int low = left ? -2 : -1, high = left ? 3 : 2;
            const bool reach = left ? hero.x_tile <= e.tile_x + 5 : hero.x_tile >= e.tile_x - 7;
            if (e.attack_cooldown == 0 && dy >= low && dy <= high && reach) {
                enemy_start_attack(e);
                if (!left) { e.pattern = kBack[std::size_t(std::clamp(e.pattern, 1, 4))]; e.direction = e.pattern; }
                return;
            }
            if (e.move_cooldown == 0) {
                if (dy > high) { e.state = EnemyState::walk; e.direction = 2; }
                else if (dy < low) { e.state = EnemyState::walk; e.direction = 1; }
                else { e.state = EnemyState::idle; e.direction = 2; }
            }
            return;
        }
        default: return; // bv.i does nothing
    }
}

void CombatWorld::boss_attack_frame(std::size_t index, HeroCombatView& hero, CombatEvents& events) {
    auto& e = enemies_[index];
    const auto& s = stat(e);
    const int f = std::abs(hero.x_tile - e.tile_x), g = std::abs(hero.y_tile - e.tile_y);
    const int k = e.frame;
    switch (e.boss) {
        case BossKind::cc: // cc.j
            if (e.pattern == 1) {
                if (k == 7)
                    for (int d = 1; d <= 4; ++d) {
                        const int x = hero.x_tile + kDx[std::size_t(d)], y = hero.y_tile + kDy[std::size_t(d)];
                        if (can_place(int(index) + 1, e.width, x, y)) {
                            unregister(int(index));
                            e.x_px = x * 16; e.y_px = y * 16; e.tile_x = x; e.tile_y = y;
                            register_actor(int(index));
                            break;
                        }
                    }
                const int nf = std::abs(hero.x_tile - e.tile_x), ng = std::abs(hero.y_tile - e.tile_y);
                if (k == 11 && nf + ng <= 1) hit_hero(index, s.attack, e.direction, hero);
            } else if (e.pattern == 2) {
                if (k == 7)
                    for (int d = 1; d <= 4; ++d)
                        boss_projectile(index, e.tile_x + kDx[std::size_t(d)], e.tile_y + kDy[std::size_t(d)], d, 3);
            } else if (k == 4 && f <= 2 && g <= 2) {
                hit_hero(index, s.attack * 2, e.direction, hero);
            }
            return;
        case BossKind::ar: // ar.j: three rays down
            if (k == 2) {
                boss_projectile(index, e.tile_x - 1, e.tile_y, e.direction, 13);
                boss_projectile(index, e.tile_x + 1, e.tile_y, e.direction, 13);
                boss_projectile(index, e.tile_x, e.tile_y + 1, e.direction, 13);
            }
            return;
        case BossKind::ag: // ag.j: fan of five rays
            if (k == 2) {
                const int x = e.tile_x, y = e.tile_y;
                for (const auto& at : {std::pair<int, int>{x - 1, y - 1}, {x + 3, y - 1}, {x, y}, {x + 2, y}, {x + 1, y + 1}})
                    boss_projectile(index, at.first, at.second, e.direction, 13);
            }
            return;
        case BossKind::bd: // bd.j: mark the hero tile, strike it three frames later
            if (k == 4) { // al.a(BB): the bd effect plays on the marked tile as the warning
                e.mark_x = hero.x_tile; e.mark_y = hero.y_tile;
                eif_effect(e.sprite, -2, e.mark_x * 16 + 8, e.mark_y * 16 + 9);
            }
            if (k == 7 && e.mark_x == hero.x_tile && e.mark_y == hero.y_tile) hit_hero(index, s.attack, e.direction, hero);
            return;
        case BossKind::cd: { // cd.j: heal whoever dropped under half health, else rotate
            if (k != 5) return;
            const auto alive = [&](int i) {
                return i >= 0 && enemies_[std::size_t(i)].state != EnemyState::dead && enemies_[std::size_t(i)].state != EnemyState::dying;
            };
            const auto below = [&](int i, int divisor) {
                const auto& t = enemies_[std::size_t(i)];
                return t.hp < stat(t).hp / divisor;
            };
            if (alive(e.partner_a) && below(e.partner_a, 2)) { heal_enemy(index, std::size_t(e.partner_a)); return; }
            if (alive(e.partner_b) && below(e.partner_b, 2)) { heal_enemy(index, std::size_t(e.partner_b)); return; }
            if (below(int(index), 2)) { heal_enemy(index, index); return; }
            const int target = e.counter == 0 ? e.partner_a : e.counter == 1 ? e.partner_b : int(index);
            if (alive(target) && below(target, 1)) heal_enemy(index, std::size_t(target));
            e.counter = (e.counter + 1) % 3;
            return;
        }
        case BossKind::ba: { // ba.j: slam with screen shake
            if (k == 6) { events.shake_x = 2; events.shake_y = 3; }
            else if (k == 7) { events.shake_x = -3; events.shake_y = -1; }
            else if (k == 8) { events.shake_x = 2; events.shake_y = -3; }
            const int top = e.tile_y - 5;
            if (k == 5 && in_area(hero, e.tile_x + 2, e.tile_x + 5, top + 1, top + 6)) {
                hit_hero(index, s.attack, 2, hero);
                e.counter = 0; e.move_delay = 2;
            }
            return;
        }
        case BossKind::ak: { // ak.j: sweep left, alternating swing sides
            const int top = e.tile_y - 4;
            if (k == 5 && e.pattern == 1 && in_area(hero, e.tile_x - 7, e.tile_x - 1, top + 1, top + 4))
                hit_hero(index, s.attack, 3, hero);
            if (k == 8 && e.pattern == 2 && in_area(hero, e.tile_x - 5, e.tile_x - 1, top + 1, top + 4)) {
                hit_hero(index, s.attack, 2, hero);
                e.counter = 0; e.move_delay = 2;
            }
            return;
        }
        case BossKind::cg: { // cg.j: the tongue descends over columns 6..9, then slams below
            if (k >= 6 && k <= 11 && k != 10) {
                const int row = e.tile_y + (k == 11 ? 4 : k - 6);
                // cg.a(ao,ae,byte): ae.a[row][6..9] == hero, so the half-step cell counts too.
                bool reached = false;
                for (int x = 6; x <= 9 && !reached; ++x) reached = occupant(x, row) == -1;
                if (reached) {
                    events.push_y += 1; // ao.a(2,16)
                    ++hero.y_tile;
                    hit_hero(index, s.attack, 2, hero);
                }
                e.body_row = row;
            } else if (k == 12) {
                e.body_row = -1;
                if (in_area(hero, 6, 9, e.tile_y + 5, e.tile_y + 8)) hit_hero(index, s.attack / 2, 2, hero);
            }
            return;
        }
        default: return;
    }
}

void CombatWorld::boss_death(std::size_t index) { // av.m overrides
    auto& e = enemies_[index];
    switch (e.boss) {
        case BossKind::cc: e.death_timer = 24; break;
        case BossKind::bv: e.death_timer = 16; break;
        case BossKind::ag: // its partners vanish with it
            for (int partner : {e.partner_a, e.partner_b})
                if (partner >= 0 && enemies_[std::size_t(partner)].state != EnemyState::dead) remove_enemy(std::size_t(partner));
            e.death_timer = 24;
            break;
        case BossKind::cg:
            e.death_timer = 12;
            e.body_row = -1;
            for (int hand : {e.partner_a, e.partner_b})
                if (hand >= 0) remove_enemy(std::size_t(hand));
            break;
        default: e.death_timer = 0; break;
    }
}

void CombatWorld::boss_finish(std::size_t index, CombatEvents& events) { // av.l overrides
    auto& e = enemies_[index];
    switch (e.boss) {
        case BossKind::cc: case BossKind::ag: case BossKind::bv:
            remove_enemy(index);
            events.start_trigger = 1; // ah.a(1)
            return;
        case BossKind::ar: { // ae.a(false): the second phase
            remove_enemy(index);
            const int ag = add_boss(BossKind::ag, 9, 5, 36, 3, 2, 2);
            const int bd = add_boss(BossKind::bd, 6, 5, 37, 2, 2, 2);
            const int cd = add_boss(BossKind::cd, 13, 5, 38, 2, 2, 2);
            if (ag >= 0) { enemies_[std::size_t(ag)].partner_a = cd; enemies_[std::size_t(ag)].partner_b = bd; }
            if (cd >= 0) { enemies_[std::size_t(cd)].partner_a = ag; enemies_[std::size_t(cd)].partner_b = bd; }
            return;
        }
        case BossKind::cg: // cg.l: the corpse stays; three rows under it become trigger 1
            if (!e.body_left) {
                for (int x = 6; x <= 9; ++x)
                    for (int y = e.tile_y; y <= e.tile_y + 2; ++y)
                        if (in_bounds(x, y)) {
                            overlay_[std::size_t(y) * std::size_t(width_) + std::size_t(x)] = 1;
                            events.overlay_writes.push_back({x, y, 1});
                        }
                e.body_left = true;
            }
            return;
        default:
            remove_enemy(index);
            return;
    }
}

void CombatWorld::hit_boss(std::size_t index, int damage, bool critical, HeroCombatView& hero,int hit_kind,int option) {
    // av.a(int,boolean,byte,boolean,byte,byte,ao): no knockback, doubled evasion roll.
    auto& e = enemies_[index];
    const auto& s = stat(e);
    damage -= s.defense;
    if (damage < 0) damage = 0;
    damage = damage * kElement[std::size_t(hero_attack_element(hero))][std::size_t(s.element)] / 10;
    if(critical) damage+=damage*hero.weapon_critical_bonus_tenths/10;
    const int ex = e.x_px + 8 + (e.width - 1) * 8, ey = e.y_px + 8 + e.draw_offset_y;
    if (rand_range(0, 99) < (s.evasion - hero.agility - hero.weapon_refine / 5 + 10) * 2) { add_popup(PopupKind::miss, 0, ex, ey, int(index)); sounds_.push_back(14); return; }
    sounds_.push_back(critical ? 15 : 13); // av.a: same sounds as al.a
    // av accepts leech/rage but does not apply cf0..4 or Deadly Blow.
    if(option==3) hero.sp=std::clamp(hero.sp+damage*30/100,0,hero.max_sp);
    else if(option==4) hero.hp=std::clamp(hero.hp+damage/2,0,hero.max_hp);
    else if(option==8) damage*=2;
    e.hp -= damage; // al.b(int)
    hit_effect(hit_kind,int(index),ex,ey);
    add_popup(PopupKind::damage, damage, ex, ey, int(index));
    if (e.hp <= 0) {
        e.state = EnemyState::dying; e.frame = -1; e.death_timer = 3;
        boss_death(index);
    }
}

void CombatWorld::update_boss(std::size_t index, HeroCombatView& hero, CombatEvents& events) {
    auto& e = enemies_[index];
    const auto& fr = frames_[std::size_t(std::clamp(e.sprite, 0, int(frames_.size()) - 1))];
    ++e.frame;
    const bool stationary = e.boss == BossKind::ag || e.boss == BossKind::bd ||
                            e.boss == BossKind::cd || e.boss == BossKind::cg;
    const bool hand = e.boss == BossKind::ba || e.boss == BossKind::ak;
    if (hand && ++e.counter > 100) e.move_delay = 0; // enraged until the next landed slam

    // al.n with the subclass overrides of h()/i()/n().
    const bool aligned = !e.partial_x && !e.partial_y;
    if (e.state == EnemyState::dying) {
        // cc.k / ag.k: death blasts y(ce.f[n]) around the dying boss, one or two per tick. cc.d/ag.d
        // run n() (q--) before k() tests q>8, so a timer of 24 emits for 23..9: 15 ticks.
        if ((e.boss == BossKind::cc || e.boss == BossKind::ag) && e.death_timer - 1 > 8) {
            // cc uses its own boss/atef (32); ag uses its cd partner's n, boss/atef/38.
            const int sprite = e.boss == BossKind::ag ? 38 : e.sprite, count = e.boss == BossKind::ag ? 2 : 1;
            for (int i = 0; i < count; ++i) {
                const int tx = e.tile_x + (e.boss == BossKind::ag ? rand_range(-2, 2) : rand_range(-1, 1));
                const int ty = e.tile_y + (e.boss == BossKind::ag ? rand_range(-2, 2) : rand_range(0, 3));
                eif_effect(sprite, -2, tx * 16 + 8, ty * 16 + 9);
            }
        }
        if (e.death_timer < 1) boss_finish(index, events); else --e.death_timer;
        // ar's next phase appends bosses and can reallocate enemies_.
        auto& corpse = enemies_[index];
        if (corpse.boss == BossKind::cg && corpse.frame >= fr.death) corpse.frame = fr.death - 1; // cg.k keeps the corpse frame
        return;
    }
    switch (e.state) {
        case EnemyState::idle: boss_think(index, hero); break;
        case EnemyState::walk:
            if (e.boss == BossKind::cc) {
                if (e.frame >= fr.walk) { enemy_rest(e, false); boss_think(index, hero); }
            } else if (stationary) {
                // h(): ag and cg end the entry on the walk length (j.k), bd and cd on idle (j.j).
                const int entry = (e.boss == BossKind::bd || e.boss == BossKind::cd) ? fr.idle : fr.walk;
                if (e.frame >= entry) e.state = EnemyState::idle; // a(1) keeps o.k
            } else if (aligned) enemy_walk_boundary(index, hero);
            break;
        case EnemyState::attack: {
            const int length = e.boss == BossKind::cc ? e.attack_length
                : e.boss == BossKind::ak ? fr.attack_by_direction[std::size_t(std::clamp(e.direction, 1, 4) - 1)]
                : fr.attack;
            if (e.frame >= length) {
                enemy_rest(e, false);
                if (e.boss == BossKind::cc) { // rotate to the next attack set
                    e.counter = (e.counter + 1) % 6;
                    cc_set_pattern(e, kCcPattern[e.counter]);
                    e.attack_cooldown = e.move_cooldown = kCcDelay[e.counter];
                }
                boss_think(index, hero);
            }
            break;
        }
        default: break;
    }
    if (e.state == EnemyState::dead) return;
    // Movement: ar flies (o.e without the wall check), hands slide vertically without collision.
    if (e.state == EnemyState::walk && !stationary && e.boss != BossKind::cc) {
        if (hand) {
            e.y_px += e.direction == 1 ? -8 : e.direction == 2 ? 8 : 0;
            e.tile_y = e.y_px >> 4; e.partial_y = (e.y_px & 15) != 0;
        } else if (e.flying || !aligned || can_step(int(index) + 1, e.direction)) {
            move_enemy(e, int(index), 8);
        } else e.state = EnemyState::idle; // o.a(): a(1) keeps o.k
    }
    // o(): attack frames, cc teleports while "walking", idle animation and cooldowns.
    switch (e.state) {
        case EnemyState::walk:
            if (e.boss == BossKind::cc) { if (e.frame == 5) cc_teleport_near(index, hero); }
            else if (e.frame >= fr.walk) e.frame = 0;
            return;
        case EnemyState::attack: boss_attack_frame(index, hero, events); return;
        default:
            if (e.frame >= fr.idle) e.frame = 0;
            if (e.attack_cooldown > 0) --e.attack_cooldown;
            if (e.move_cooldown > 0) --e.move_cooldown;
            return;
    }
}

} // namespace combat
