// Field guardian (p) and hero statuses (cf), ported from p.a(boolean,byte,int,int),
// p.e(), p.a(byte,byte) and the status handling of ao.d / ao.a(al,short,byte).
#include "combat.hpp"

#include <algorithm>
#include <cstdlib>

namespace combat {
namespace {

// p.b: skill cooldown, p.i: channel loops before the effect, p.d: effect length; index id*3+skill.
constexpr int kCooldown[18] = {56, 280, 280, 220, 160, 270, 56, 270, 220, 80, 270, 270, 80, 270, 220, 80, 270, 270};
constexpr int kLoops[18] = {0, 0, 10, 4, 4, 10, 0, 4, 0, 4, 10, 8, 0, 6, 6, 0, 10, 8};
constexpr int kEffectLength[18] = {16, 9, 161, 9, 10, 81, 16, 9, 13, 25, 81, 8, 31, 81, 16, 31, 87, 8};
// cf.a durations of statuses 5 (attack up), 6 (defense up), 7 (poison).
constexpr int kAttackUp = 140, kDefenseUp = 160, kPoison = 80;

} // namespace

// y.a: lifetime of each effect kind.
constexpr int kEffectDuration[11] = {-1, 4, 8, 6, 10, 11, 7, 9, 6, 4, 3};

void CombatWorld::spawn_effect(int kind, int x_px, int y_px) {
    if (kind < 1 || kind > 10) return;
    effects_.push_back({kind, x_px, y_px, 0, kEffectDuration[kind]});
}

void CombatWorld::spawn_effect_at(int kind, int distance, int side) {
    const int d = guardian_.direction;
    int x = guardian_.x * 16 + kDx[std::size_t(d)] * 16 * distance;
    int y = guardian_.y * 16 + kDy[std::size_t(d)] * 16 * distance;
    if (side) { x += kDx[std::size_t(side)] * 16; y += kDy[std::size_t(side)] * 16; }
    spawn_effect(kind, x, y);
}

void CombatWorld::add_aura(int target, int delay, int duration) { // new bj(delay, duration, f, m)
    auras_.push_back({target, delay, duration, guardian_.id, guardian_.skill, 0});
}

void CombatWorld::age_effects() {
    for (auto& e : effects_) ++e.frame;
    effects_.erase(std::remove_if(effects_.begin(), effects_.end(),
        [](const WorldEffect& e) { return e.frame >= e.duration; }), effects_.end());
    for (auto& a : auras_) { if (a.delay > 0) --a.delay; else ++a.frame; }
    auras_.erase(std::remove_if(auras_.begin(), auras_.end(), [&](const Aura& a) {
        return a.frame >= a.duration || (a.target >= 0 && (std::size_t(a.target) >= enemies_.size() ||
               enemies_[std::size_t(a.target)].state == EnemyState::dead));
    }), auras_.end());
}

int CombatWorld::guardian_cooldown(int id, int skill) const {
    if (id < 0 || id > 5 || skill < 0 || skill > 2) return 0;
    return guardian_cooldowns_[std::size_t(id)][std::size_t(skill)];
}

int CombatWorld::guardian_cooldown_full(int id, int skill) {
    return id < 0 || id > 5 || skill < 0 || skill > 2 ? 0 : kCooldown[id * 3 + skill];
}

void CombatWorld::ensure_cooldowns(const HeroCombatView& hero) {
    // p(short,short,byte): c[0..2] all start at b[id*3+i]; assigning a skill later keeps its
    // timer, which only counts down while the skill sits on a key (p.e).
    const int id = hero.guardian_id;
    if (id < 0 || id > 5 || guardian_seen_[std::size_t(id)]) return;
    guardian_seen_[std::size_t(id)] = true;
    for (int skill = 0; skill < 3; ++skill) guardian_cooldowns_[std::size_t(id)][std::size_t(skill)] = kCooldown[id * 3 + skill];
}

int CombatWorld::guardian_element() const { // p.a()
    return guardian_.id < 0 ? 0 : guardian_.id % 3 + 1;
}

bool CombatWorld::summon_guardian(const HeroCombatView& hero, bool calm) {
    // ao.a(boolean) checks only ae.c and p.i == 0: unlike ao.k it does not refuse a dying hero (o.h 6).
    if (!hero.combat_map || hero.guardian_id < 0 || hero.guardian_id > 5 || guardian_.state != 0) return false;
    ensure_cooldowns(hero);
    const int skill = calm ? hero.guardian_calm : hero.guardian_wild;
    if (skill < 0 || skill > 2) return false;
    auto& cooldown = guardian_cooldowns_[std::size_t(hero.guardian_id)][std::size_t(skill)];
    if (cooldown != 0) return false;
    const int key = hero.guardian_id * 3 + skill;
    cooldown = kCooldown[key];
    guardian_ = FieldGuardian{};
    guardian_.id = hero.guardian_id; guardian_.level = hero.guardian_level;
    guardian_.skill = skill; guardian_.state = 1; guardian_.anim = -1;
    guardian_.loops = guardian_.loops_total = kLoops[key];
    const int dir = std::clamp(hero.direction, 1, 4);
    guardian_.direction = dir;
    guardian_.x = hero.x_tile + kDx[std::size_t(dir)];
    guardian_.y = hero.y_tile + kDy[std::size_t(dir)];
    return true;
}

void CombatWorld::hit_enemy_element(std::size_t index, int damage, int element) {
    // al.a(int,byte) / av.a(int,byte): no evasion roll, element scaling only.
    auto& e = enemies_[index];
    if (e.state == EnemyState::dying || e.state == EnemyState::dead || e.ghost) return;
    const auto& s = stat(e);
    if (e.boss == BossKind::none) {
        e.provoked = true; e.hidden = false;
        e.status_elapsed[0] = -1; // al.a(int,byte) -> al.t(): elemental hits wake Sleep too
        e.status_order.erase(std::remove(e.status_order.begin(), e.status_order.end(), 0), e.status_order.end());
    }
    if (damage < 0) damage = 0;
    damage = damage * kElement[std::size_t(std::clamp(element, 0, 3))][std::size_t(s.element)] / 10;
    e.hp -= damage;
    // al.a(IB)/av.a(IB) do not call as.a: guardian hits leave the HUD target alone.
    hit_effect(1,int(index),e.x_px+8+(e.width-1)*8,e.y_px+8+e.draw_offset_y);
    add_popup(PopupKind::damage, damage, e.x_px + 8 + (e.width - 1) * 8, e.y_px + 8 + e.draw_offset_y, int(index));
    if (e.boss == BossKind::none) { e.shake = 4; e.shake_direction = 4; }
    if (e.hp <= 0) {
        e.state = EnemyState::dying; e.frame = 0; e.death_timer = 3; // al.a(IB) and av.a(IB) both set k = 0
        if (e.boss != BossKind::none) boss_death(index);
    }
}

void CombatWorld::guardian_strike(int x, int y, int damage) {
    const int who = occupant(x, y);
    if (who > 0) hit_enemy_element(std::size_t(who - 1), damage, guardian_element());
}

void CombatWorld::guardian_strike_dir(int direction, int distance, int damage) { // ck.a(byte,byte) from p
    guardian_strike(guardian_.x + kDx[std::size_t(direction)] * distance,
                    guardian_.y + kDy[std::size_t(direction)] * distance, damage);
}

void CombatWorld::guardian_wave(int start, int step, int damage) { // p.a(int,int,int,int)
    const int t = guardian_.effect_tick - start;
    if (t < 0 || t % step != 0) return;
    const int ring = t / step;
    if (ring == 0) { guardian_strike_dir(guardian_.direction, 0, damage); return; }
    for (int d = 1; d <= 4; ++d) guardian_strike_dir(d, ring, damage);
}

void CombatWorld::guardian_line(const std::array<int, 4>& ticks, int damage) {
    // p.g / p.h: the strike travels 0..3 tiles ahead on the listed ticks (+4 for the echo).
    const int e = guardian_.effect_tick;
    for (int distance = 0; distance < 4; ++distance)
        if (e == ticks[std::size_t(distance)] || e == ticks[std::size_t(distance)] + 4)
            guardian_strike_dir(guardian_.direction, distance, damage);
}

namespace {
// p.a(byte,byte), p.i(), p.j(): the WAV each guardian skill plays on its effect ticks.
int guardian_sound(int id, int skill, int e) {
    switch (id) {
        case 0: return skill == 0 ? (e == 0 || e == 4 || e == 8 ? 16 : 0) : e == 0 ? 21 : 0;
        case 1: return e == 0 ? 20 : 0;
        case 2: return skill == 0 ? (e == 0 || e == 3 || e == 7 ? 17 : 0) : e == 0 ? 21 : 0;
        case 3: return skill == 0 ? (e == 0 || e == 6 || e == 12 ? 16 : 0) : skill == 1 ? (e % 8 == 0 ? 16 : 0) : e == 0 ? 16 : 0;
        case 4: return skill == 0 ? (e % 6 == 0 && e <= 24 ? 18 : 0) : skill == 1 ? (e % 8 == 0 ? 18 : 0) : e == 0 ? 20 : 0;
        case 5: return skill == 0 ? (e % 6 == 0 && e <= 24 ? 17 : 0) : skill == 1 ? (e % 8 == 0 ? 17 : 0) : e == 0 ? 17 : 0;
        default: return 0;
    }
}
}
void CombatWorld::guardian_effect(HeroCombatView& hero, CombatEvents& events) { // p.a(byte,byte)
    auto& g = guardian_;
    const int e = g.effect_tick, key = g.id * 3 + g.skill;
    const int lvl = g.level, will = hero.base_will; // p.* read ao.g:S, never the gear bonus ao.p
    if (const int sound = guardian_sound(g.id, g.skill, e)) sounds_.push_back(sound);
    const auto storm = [&](int damage) { // p.b(2) + p.a(short,int): two random cells near the guardian
        for (int i = 0; i < 2; ++i) {
            const int a = next_int() % 3;
            const int b = next_int() % (2 - std::abs(a) + 1);
            g.storm[std::size_t(i * 2)] = a; g.storm[std::size_t(i * 2 + 1)] = b;
        }
        if (damage > 0 && e < 80)
            for (int i = 0; i < 2; ++i) guardian_strike(g.x + g.storm[std::size_t(i * 2)], g.y + g.storm[std::size_t(i * 2 + 1)], damage);
    };
    const auto instant_kill = [&]() { // p.a(byte): same-element ordinary enemies ahead die outright
        if (e != 0) return;
        for (int distance = 0; distance <= 3; ++distance) {
            const int who = occupant(g.x + kDx[std::size_t(g.direction)] * distance, g.y + kDy[std::size_t(g.direction)] * distance);
            if (who <= 0) continue;
            auto& target = enemies_[std::size_t(who - 1)];
            if (target.boss != BossKind::none || stat(target).element != guardian_element()) continue;
            // al.c(byte) -> al.b(max HP): plain HP loss and its popup, no sparks, shake or targeting.
            if (target.state == EnemyState::dying || target.state == EnemyState::dead) continue;
            target.hidden = false; // al.c: e = false
            const int damage = stat(target).hp;
            target.hp -= damage;
            add_popup(PopupKind::damage, damage, target.x_px + 8 + (target.width - 1) * 8, target.y_px + 8 + target.draw_offset_y, who - 1);
            if (target.hp <= 0) { target.state = EnemyState::dying; target.frame = -1; target.death_timer = 3; } // al.b: k = -1
        }
    };
    switch (g.id) {
        case 0:
            if (g.skill == 0) {
                if (e == 0 || e == 5) spawn_effect_at(1, 0, 0);
                if (e == 1 || e == 6) spawn_effect_at(1, 1, 0);
                if (e == 2 || e == 7) spawn_effect_at(1, 2, 0);
                if (e == 4 || e == 8) {
                    spawn_effect_at(2, 3, 0);
                    spawn_effect_at(2, 3, kSideE[std::size_t(g.direction)]);
                    spawn_effect_at(2, 3, kSideF[std::size_t(g.direction)]);
                }
                const int damage = lvl * 3 + 40 + will;
                guardian_line({{1, 2, 3, 5}}, damage);
                if (e == 8 || e == 12) {
                    const int d = g.direction;
                    for (const auto& side : {kSideE, kSideF})
                        guardian_strike(g.x + kDx[std::size_t(d)] * 3 + kDx[std::size_t(side[std::size_t(d)])],
                                        g.y + kDy[std::size_t(d)] * 3 + kDy[std::size_t(side[std::size_t(d)])], damage);
                }
            } else if (g.skill == 1) { if (e == 0) { if(!status_.attack_up) status_.order.push_back(5); status_.attack_up = kAttackUp; add_aura(-1, 0, 8); } }
            else {
                if (e == 0) { status_.thorns = true; add_aura(-1, 0, 160); }
                if (e == kEffectLength[key]) status_.thorns = false;
            }
            break;
        case 1:
            if (g.skill == 0 && e == 0) { hero.hp = std::min(hero.max_hp, hero.hp + hero.max_hp * 30 / 100); add_aura(-1, 0, 8); }
            if (g.skill == 1 && e == 0) { hero.sp = std::min(hero.max_sp, hero.sp + hero.max_sp * 20 / 100); add_aura(-1, 0, 9); }
            if (g.skill == 2) {
                if (e == 0) {
                    status_.regen_double = true;
                    add_aura(-1, 0, 80);
                    for (int delay : {4, 24, 44}) { add_aura(-1, delay, 8); auras_.back().skill = 0; }
                }
                if (e == kEffectLength[key]) status_.regen_double = false;
            }
            break;
        case 2:
            if (g.skill == 0) {
                if (e >= 0 && e <= 5) { events.shake_x = rand_range(-4, 4); events.shake_y = rand_range(-4, 4); }
                if (e <= 7) spawn_effect_at(4, e % 4, 0); // p.j: kind 4 travelling 0..3 tiles, twice
                guardian_line({{2, 3, 4, 5}}, lvl * 3 + 35 + will * 3 / 2);
            } else if (g.skill == 1) { if (e == 0) { if(!status_.defense_up) status_.order.push_back(6); status_.defense_up = kDefenseUp; add_aura(-1, 0, 8); } }
            else if (e == 0) { status_ = HeroStatus{}; add_aura(-1, 0, 12); } // cure: remove every status
            break;
        case 3: case 4: case 5: {
            const int step = g.id == 3 ? 3 : 5;
            const int ring_kind = g.id == 3 ? 5 : g.id == 4 ? 7 : 4;
            if (g.skill == 0) { // p.a(byte,int,int): visual rings start at 0/6/12
                for (int start : {0, 6, 12}) {
                    const int t = e - start;
                    if (t < 0 || t % step != 0) continue;
                    const int ring = t / step;
                    if (ring == 0) spawn_effect(ring_kind, g.x * 16, g.y * 16);
                    else if (ring <= 3)
                        for (int d = 1; d <= 4; ++d)
                            spawn_effect(ring_kind, g.x * 16 + kDx[std::size_t(d)] * 16 * ring,
                                         g.y * 16 + kDy[std::size_t(d)] * 16 * ring);
                }
                const int damage = lvl * 3 + 35 + will;
                for (int start : {4, 10, 16}) guardian_wave(start, step, damage);
            } else if (g.skill == 1) {
                const int storm_kind = g.id == 3 ? 6 : g.id == 4 ? 8 : 9;
                const auto storm_effects = [&]() { // p.a(int,int,short,byte)
                    if (e >= 80) return;
                    for (int i = 0; i < 2; ++i)
                        spawn_effect(storm_kind, (g.x + g.storm[std::size_t(i * 2)]) * 16,
                                     (g.y + g.storm[std::size_t(i * 2 + 1)]) * 16);
                };
                if (g.id == 4) { // invulnerability while the hero stays within 2 tiles
                    storm(0);
                    storm_effects();
                    status_.invulnerable = std::abs(g.x - hero.x_tile) + std::abs(g.y - hero.y_tile) <= 2;
                    if (e == kEffectLength[key]) status_.invulnerable = false;
                } else { storm(lvl + 45 + will * 3 / 2); storm_effects(); }
            } else if (g.id == 4) {
                if (e == 0) {
                    hero.hp = std::min(hero.max_hp, hero.hp + hero.max_hp * 20 / 100);
                    hero.sp = std::min(hero.max_sp, hero.sp + hero.max_sp * 20 / 100);
                    add_aura(-1, 0, 15);
                }
            } else {
                if (e == 0) // p.a(short): matching enemies get an aura, other tiles a kind-10 mark
                    for (int distance = 1; distance <= 3; ++distance) {
                        const int who = occupant(g.x + kDx[std::size_t(g.direction)] * distance,
                                                 g.y + kDy[std::size_t(g.direction)] * distance);
                        const bool match = who > 0 && enemies_[std::size_t(who - 1)].boss == BossKind::none &&
                                           stat(enemies_[std::size_t(who - 1)]).element == guardian_element();
                        if (match) add_aura(who - 1, 0, g.id == 3 ? 7 : 9);
                        else spawn_effect_at(10, distance, 0);
                    }
                instant_kill();
            }
            break;
        }
        default: break;
    }
    if (e == kEffectLength[key]) g.done = true;
}

void CombatWorld::update_guardian(HeroCombatView& hero, CombatEvents& events) { // p.e
    ensure_cooldowns(hero);
    auto& g = guardian_;
    // p.a:S is the guardian's own level: XP gained during the summon (ao.f -> p.a(I)) raises it
    // for the following strikes. The summon is always the equipped guardian (ao.a(p) refuses swaps).
    if (g.state != 0 && hero.guardian_id == g.id) g.level = hero.guardian_level;
    if (hero.guardian_id >= 0 && hero.guardian_id <= 5) {
        auto& cd = guardian_cooldowns_[std::size_t(hero.guardian_id)];
        for (int skill : {hero.guardian_calm, hero.guardian_wild})
            if (skill >= 0 && skill <= 2 && (g.state == 0 || g.skill != skill) && cd[std::size_t(skill)] > 0)
                --cd[std::size_t(skill)];
    }
    switch (g.state) {
        case 0: return;
        case 1:
            if (++g.anim >= 10) {
                if (g.loops > 0) g.state = 2; else { g.state = 3; g.effect_tick = 0; }
                g.anim = 0;
            }
            break;
        case 2:
            ++g.anim; --g.loops;
            if (g.anim >= 4) g.anim = 0;
            if (g.loops <= 0) { g.state = 3; g.anim = 0; g.effect_tick = 0; }
            break;
        case 3:
            if (++g.anim >= 11) { g.anim = 11; if (g.done) { g.state = 0; g.direction = 0; } }
            break;
        default: break;
    }
    if (g.state == 3) { guardian_effect(hero, events); ++g.effect_tick; }
}

void CombatWorld::update_status(HeroCombatView& hero) { // ao.d: cf timers and poison drain
    if (status_.attack_up > 0) --status_.attack_up;
    if (status_.defense_up > 0) --status_.defense_up;
    if (status_.poison > 0) {
        --status_.poison;
        if (++status_.poison_tick % 10 == 0 && hero.hp > 1) {
            const int drain = std::min(hero.max_hp / 25, hero.hp - 1);
            hero.hp -= drain;
            // ao.d displays a separate amount, deliberately not the actual HP drain.
            add_popup(PopupKind::damage, -(hero.max_hp/12), hero.x_tile * 16 + 8, hero.y_tile * 16 + 8, -1);
        }
    }
    status_.order.erase(std::remove_if(status_.order.begin(),status_.order.end(),[&](int id){
        return id==5?!status_.attack_up:id==6?!status_.defense_up:!status_.poison;
    }),status_.order.end());
}

} // namespace combat
