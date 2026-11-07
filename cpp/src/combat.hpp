#pragma once
// Field combat ported from al (enemy), o/ck (actor base), ao (hero attack/hit),
// i (tile projectiles), ae (spawn queue, ground drops) and n.a() (pickup).
// Boss subclasses of av and guardian skills (p) are included; remaining
// status/weapon options and presentation gaps are tracked in ROADMAP.md.

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace combat {

// u.a/u.b: direction vectors for 0..8 (1 up, 2 down, 3 left, 4 right, 5..8 diagonals).
inline constexpr std::array<int, 9> kDx{{0, 0, 0, -1, 1, -1, 1, -1, 1}};
inline constexpr std::array<int, 9> kDy{{0, -1, 1, 0, 0, -1, -1, 1, 1}};
// u.c/u.d: the two diagonals adjacent to a cardinal direction (sweep hits).
inline constexpr std::array<int, 5> kDiagC{{0, 6, 7, 5, 8}};
inline constexpr std::array<int, 5> kDiagD{{0, 5, 8, 7, 6}};
// u.e/u.f: side steps used by the chase path.
inline constexpr std::array<int, 5> kSideE{{0, 4, 3, 1, 2}};
inline constexpr std::array<int, 5> kSideF{{0, 3, 4, 2, 1}};
// u.g: opposite direction.
inline constexpr std::array<int, 5> kBack{{0, 2, 1, 4, 3}};
inline constexpr std::array<int,9> kOptionChance{{20,16,6,13,13,10,10,10,10}};
inline constexpr std::array<int,9> kOptionStatus{{0,1,-1,-1,-1,4,3,2,-1}};
// u.a[][]: attacker element x defender element, in tenths.
inline constexpr std::array<std::array<int, 4>, 4> kElement{{
    {{10, 10, 10, 10}}, {{10, 10, 6, 13}}, {{10, 13, 10, 6}}, {{10, 6, 13, 10}}}};
// j.a: per enemy sprite the attack frame that deals the hit (index = frame + 1).
inline constexpr std::array<int, 42> kEnemyHitFrame{{
    3, 2, 6, 2, 2, 1, 3, 4, 3, 2, 3, 4, 2, 3, 2, 2, 2, 3, 3, 3, 3,
    3, 6, 3, 3, 3, 2, 2, 2, 3, 3, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}};

struct DropRule { int type{}, id{}, chance{}; };

// One record of /enm/data{0,1,2}; j.a([BBB). The file index is the
// playthrough counter n.g clamped to 2 (New Game+ uses the stronger tables).
struct EnemyStats {
    int name_text{};
    int size{};          // j.a:B, 2 = two tiles wide and not respawned at the right edge
    int respawn{};       // j.b:B, 2 = never respawns
    int element{};       // j.c:B
    int attack_kind{};   // j.d:B, 0/1 melee (1 may poison), 2 reach 3, 3 projectile
    bool chase_far{};    // j.a:Z, aggro range 100 once provoked
    bool half_damage{};  // j.b:Z
    bool summoner{};     // j.c:Z
    bool ambush{};       // j.d:Z, hidden until it attacks or is hit
    int summon_element{};// j.e:B
    int level{};         // j.f:B
    int hp{}, attack{}, defense{}, evasion{}; // j.a..d:S
    int aggro_range{};   // j.g:B
    int move_delay{};    // j.h:B
    int attack_delay{};  // j.i:B
    int respawn_delay{}; // j.e:S, ticks
    std::vector<DropRule> drops;
};
std::vector<EnemyStats> parse_enemy_table(const std::vector<std::uint8_t>& bytes);

// Animation frame counts of enm/spr/{id}: j.j/k/l from ce.e[id*12 + 0/4/8][0].
// Bosses (j.b): idle/walk/attack/death from ce.h[slot*16 + 0/4/12/8]; cc also has three
// alternative attack sets (boss/spr types 3..5) and ak uses the per-direction attack length.
struct EnemyFrames {
    int idle{1}, walk{1}, attack{1}, death{1};
    int effect{3}; // frame count of enm/atef or boss/atef (projectile lifetime, y.a:S)
    std::array<int, 4> attack_by_direction{{1, 1, 1, 1}};
    std::array<int, 3> alt_attack{{1, 1, 1}};
};

// av subclasses created by ae for maps 11 (cc), 13 (ar, then ag+bd+cd), 15 (cg+ba+ak), 82 (bv).
enum class BossKind : std::uint8_t { none, cc, ar, ag, bd, cd, ba, ak, cg, bv };

enum class EnemyState : std::uint8_t { idle = 1, walk = 2, attack = 3, knockback = 4, dying = 5, dead = 6 };

struct Enemy {
    int sprite{};      // al.m: enm/spr id (EVT code)
    int stats{};       // al.n: index into EnemyStats
    int origin_x{}, origin_y{}; // al.a/b:I, respawn point
    int x_px{}, y_px{};
    int tile_x{}, tile_y{};
    bool partial_x{}, partial_y{};
    int width{1};
    EnemyState state{EnemyState::idle};
    int direction{2};  // o.j: facing (sprite, chase facing test, al.j projectile)
    int knock_direction{}; // o.i when knockback moved it off o.j; 0 = same as direction
    int heading() const { return knock_direction ? knock_direction : direction; } // o.i: steps, attack reach
    int frame{};       // o.k
    int hp{};
    std::array<int,5> status_elapsed{{-1,-1,-1,-1,-1}}; // cf0..4, -1 inactive; refresh sets0
    std::vector<int> status_order; // o.b insertion order; rendering traverses backwards
    int wander{};      // al.r
    int move_cooldown{}, attack_cooldown{}; // al.o / al.p
    int death_timer{}; // al.q
    int shake{}, shake_direction{}; // al.s / al.t
    int knockback{};   // al.l
    int summon_timer{-10};
    bool provoked{};   // al.d
    bool hidden{};     // al.e
    int target{};      // al.c: 0 none, 1 hero, 2 the channelling guardian
    bool on_screen{true};
    int generation{};   // port: bumped on every spawn into a slot (projectile owner identity)
    int move_delay{};    // j.h copy; ba/ak rewrite it (enrage after 100 idle ticks)
    // Boss state (av subclasses).
    BossKind boss{BossKind::none};
    int pattern{};       // cc.w attack set, ak.w swing side
    int counter{};       // cc.v rotation, cd.v heal rotation, cg.v attack count, ba/ak.v idle ticks
    int attack_length{}; // cc.x
    int mark_x{-1}, mark_y{-1}; // bd.v/w target tile
    int partner_a{-1}, partner_b{-1}; // ag: cd,bd; cd: ag,bd; cg: ba,ak
    int draw_offset_y{}; // ba -80, ak -64
    int draw_direction{};// fixed sprite direction when non-zero
    bool ghost{};        // hands of cg never register in the occupancy grid
    bool flying{};       // ar.a() ignores walls
    bool body_left{};    // cg.g: corpse already turned into trigger tiles
    int body_row{-1};    // cg tongue row currently occupying columns 6..9
};

struct GroundDrop { int x{}, y{}, type{}, id{}, count{}, ttl{120}; int gold() const { return type == -1 ? id * 100 + count : 0; } };

enum class PopupKind : std::uint8_t { damage = 7, miss = 2, level_up = 3 };
// aw7/aw2 live in the owner's o.a vector: target -1 = hero, >= 0 = enemy slot (with its spawn
// generation), -2 = fixed world point. Renderers re-anchor owned popups on the actor each frame.
struct Popup { PopupKind kind{}; int value{}; int x_px{}, y_px{}; int age{}; int duration{4}; int target{-2}, generation{}; };

struct Projectile {
    int x{}, y{}, direction{};
    int range{};       // i.g
    int step_frame{2}; // i.h
    int frame{};       // y.b
    int duration{3};   // eif frame count
    bool from_hero{};
    bool pierce{};
    bool spent{};
    int source{-1};    // enemy index for enemy projectiles
    int source_generation{}, source_stats{-1}; // the spawn that fired it; the slot may be reused
    int damage{}, status{-1}; bool critical{};
    int effect{};      // eif id for drawing (enemy sprite id or hero spell index)
};

// Hero data the port already tracks elsewhere; combat reads and writes it.
struct HeroCombatView {
    int x_tile{}, y_tile{};
    bool partial_x{}, partial_y{};
    int direction{2};
    int character_class{6};
    int level{1};
    int hp{}, max_hp{}, sp{}, max_sp{};
    int attack{}, defense{}, agility{}, will{};
    int weapon_power{};      // e.a:S of the equipped weapon (combo SP cost)
    int weapon_critical_chance{},weapon_critical_bonus_tenths{}; // l.a/l.b signed bytes
    int weapon_hit_option{-1},shield_hit_option{-1}; // t.c, weapon before shield fallback
    int hp_attribute{};      // ao.e:S+n:B, HP regenerated per field period
    int base_will{};         // ao.g:S without gear: the thorns reflect (ao.a(al,short,byte))
    int max_combo{1};        // ao.z
    bool moving{};           // ao.h == 2 (walking regenerates slower)
    // Equipped guardian (ao.a`: p), -1 when none.
    int guardian_id{-1}, guardian_level{1}, guardian_calm{-1}, guardian_wild{-1};
    int accessory_evasion{}; // a[2].e:B
    int weapon_refine{};     // a[0].e:B: al/av subtract e/5 from the target's evasion
    bool combat_map{true};   // ae.c: peaceful maps (kinds 1/5/9/15) block attacks and guardians
    std::array<int,3> magic_frames{{2,2,7}}; // y.a of Kail's c3/s/ea2..ea4 (same in all three JARs)
};
// ao.a():p -> p.a():byte. Normal attacks use the equipped guardian's element.
inline int hero_attack_element(const HeroCombatView& hero) {
    return hero.guardian_id>=0 && hero.guardian_id<6?hero.guardian_id%3+1:0;
}
inline int hero_critical_chance(const HeroCombatView& hero) {
    return hero.agility/3+hero.will/10+hero.weapon_critical_chance;
}

// p: a summoned guardian performs one skill in front of the hero and then disappears.
struct FieldGuardian {
    int id{-1}, level{1};
    int state{};       // p.i: 0 hidden, 1 rising, 2 channelling, 3 effect
    int anim{};        // p.b
    int loops{}, loops_total{}; // p.l / p.k
    int effect_tick{}; // p.e
    int skill{-1};     // p.m
    bool done{};       // p.d
    int x{}, y{}, direction{};
    std::array<int, 4> storm{}; // p.j: two random offsets within manhattan 2
};

// y(short,short,byte) spawned by guardian skills: kind 1..10, pixel position of the tile.
struct WorldEffect { int kind{}, x_px{}, y_px{}, frame{}, duration{}; };
struct BattleEffect { int kind{},target{-1},x_px{},y_px{},frame{},duration{3}; }; // aw 1/5/6, death y 100..102
// aw type 9 / y(BB[B): an enemy's or boss's own atef EIF (ce.f[n]) played once; kind = base + sprite.
inline constexpr int kEifEffectBase = 1000;
struct DamageNumberFrame {int strip{},width{},height{},advance{};};
inline DamageNumberFrame damage_number_frame(int value,int age) {
    const bool large=age<2;
    return {(large?7:5)+(value<0?1:0),large?9:7,large?14:9,large?8:6};
}
// bj: aura drawn on the hero (target -1) or on an enemy slot, after `delay` ticks.
struct Aura { int target{-1}, delay{}, duration{}, guardian{}, skill{}, frame{}; };

// cf statuses on the hero (ids follow cf.a durations: 5 attack up, 6 defense up, 7 poison).
struct HeroStatus {
    std::vector<int> order;
    int attack_up{}, defense_up{}, poison{}; // remaining ticks
    int poison_tick{};
    bool thorns{}, regen_double{}, invulnerable{};
};

struct OverlayWrite { int x{}, y{}; std::int8_t value{}; };

struct CombatEvents {
    int experience{};  // already multiplied the way ao.f(int) expects (before x4)
    int gold{};
    int start_trigger{}; // ah.a(byte): start the type-0 event with this trigger ID
    std::vector<OverlayWrite> overlay_writes; // cg corpse becomes trigger 1
    int push_x{}, push_y{}; // ao.a(byte,byte): hero pushed by whole tiles
    int shake_x{}, shake_y{}; // ae.e/f one-frame screen offset
    std::vector<std::pair<int, int>> items; // type,id picked up
    bool hero_died{};
    std::vector<std::string> messages;
};

class CombatWorld {
public:
    void reset(int width, int height, std::vector<std::int8_t> overlay,
               const std::vector<EnemyStats>* stats, std::uint32_t seed = 0x1234567u);
    void set_enemy_frames(int sprite, EnemyFrames frames);
    // al.a(Graphics,II) sets al.f from the screen bounds; the port passes the visible world
    // rectangle (pixels) before each tick. Without one every enemy counts as visible.
    void set_visible_area(int x0,int y0,int x1,int y1) {visible_={{x0,y0,x1,y1}};has_visible_=true;}
    void set_death_effect_frames(int kind,int count) {if(kind>=0 && kind<3) death_effect_frames_[kind]=std::max(1,count);}
    // EVT spawn records: x, y, signed id; ae.c(byte[],int) queues them with delay 0.
    void queue_spawn(int sprite, int stats_index, int x, int y, int delay = 0);
    // Immediate placement without the spawn scatter (tests, scripted spawns).
    bool place_enemy(int sprite, int stats_index, int x, int y) { return spawn_enemy(x, y, sprite, stats_index, false, 0, 1); }
    void set_blocked_tiles(std::vector<std::pair<int, int>> tiles) { npc_tiles_ = std::move(tiles); }

    // One game tick (ae.b + hero attack step). Hero is passed by reference so
    // HP/SP changes apply immediately.
    void tick(HeroCombatView& hero, CombatEvents& events);

    // FIRE: n.a(boolean). Returns true when the hero entered/queued an attack. When a new
    // attack starts, *face receives the ao.i() auto-facing direction (or stays unchanged).
    bool request_attack(const HeroCombatView& hero, bool skill, int* face = nullptr);
    bool hero_attacking() const { return hero_state_ == 3 || hero_dying_; }
    int hero_sprite_state() const; // ao.e(): 3..10 while attacking
    int hero_frame() const { return hero_render_frame_; }
    std::pair<int,int> hero_hit_offset() const {return {hero_shake_?kDx[hero_shake_direction_]*2:0,hero_shake_?kDy[hero_shake_direction_]*2:0};}
    const Enemy* target_enemy() const {
        return target_ticks_>0 && target_>=0 && std::size_t(target_)<enemies_.size() &&
            enemies_[target_].state!=EnemyState::dead?&enemies_[target_]:nullptr;
    }
    const std::vector<BattleEffect>& battle_effects() const {return battle_effects_;}
    // ao.r: a level gained adds aw(3) (blinking LEVEL UP, h[3]=11) and aw(4) (char/lvup.eif, h[4]=9) on the hero.
    void advance_effects_for_snapshot() {for(auto& effect:battle_effects_) ++effect.frame;}
    void add_level_up_effects() {battle_effects_.push_back({3,-1,0,0,0,11});battle_effects_.push_back({4,-1,0,0,0,9});}
    int hero_dash_offset() const { return dash_; } // ao.y, tiles
    bool hero_dying() const { return hero_dying_; }
    void clear_hero() {
        hero_state_ = 1; hero_dying_ = hero_dashing_ = false;
        hero_frame_ = hero_render_frame_ = hero_shake_ = death_timer_ = recover_ = dash_ = strike_damage_ = 0;
        strike_status_ = -1; strike_critical_ = false;combo_.fill(0);combo_index_ = -1;
    }
    // o.c(): drop every aw/bj attached to an actor (-1 hero, >= 0 enemy); cf statuses stay.
    void clear_attached_effects(int target) {
        const auto owned = [target](const auto& x) { return x.target == target; };
        battle_effects_.erase(std::remove_if(battle_effects_.begin(), battle_effects_.end(), owned), battle_effects_.end());
        popups_.erase(std::remove_if(popups_.begin(), popups_.end(), owned), popups_.end());
        auras_.erase(std::remove_if(auras_.begin(), auras_.end(), owned), auras_.end());
    }
    // ah.t: scripted collision change of one tile (shared ae grid in the original).
    void set_collision(int x, int y, std::int8_t value) {
        if (in_bounds(x, y)) overlay_[std::size_t(y) * std::size_t(width_) + std::size_t(x)] = value;
    }
    // ao.a() on every map entry: v = min(67 + level, 100), w = 21.
    void restart_regen(int level) { hp_regen_timer_ = std::min(67 + level, 100); sp_regen_timer_ = 21; }
    // A new/loaded hero must not inherit the previous session's combat state.
    void reset_hero_state() {
        clear_hero();status_ = HeroStatus{};guardian_ = FieldGuardian{};
        guardian_cooldowns_ = {};guardian_seen_ = {};effects_.clear();auras_.clear();
        battle_effects_.clear();target_=-1;target_ticks_=0;
        hp_regen_timer_ = 68;sp_regen_timer_ = 21;max_combo_ = 1;
        shake_x_ = shake_y_ = 0;
    }

    // n.a(): pickup at the hero tile; returns true when something was taken.
    // ao.d offsets 604-625: a walk stopped by an enemy selects it for the HUD (as.a(al,false)).
    void select_contact(int x, int y) {
        const int id = occupant(x, y);
        if (id > 0 && std::size_t(id) <= enemies_.size() && enemies_[std::size_t(id - 1)].state != EnemyState::dead)
            select_target(std::size_t(id - 1));
    }
    bool try_pickup(const HeroCombatView& hero, CombatEvents& events,
                    const std::function<bool(int,int)>& bag_has_room);

    const std::vector<Enemy>& enemies() const { return enemies_; }
    const std::vector<GroundDrop>& drops() const { return drops_; }
    const std::vector<Popup>& popups() const { return popups_; }
    // bw.a(id,false) requests since the last call: 13 hit, 14 miss/whiff, 15 critical, 16..21 guardians.
    std::vector<int> take_sounds() { auto out = std::move(sounds_); sounds_.clear(); return out; }
    const std::vector<Projectile>& projectiles() const { return projectiles_; }
    const std::vector<EnemyStats>* stats() const { return stats_; }
    bool occupied_by_enemy(int x, int y) const;
    // ao.a(boolean): summon the equipped guardian's calm (true) or wild skill.
    bool summon_guardian(const HeroCombatView& hero, bool calm);
    const FieldGuardian& guardian() const { return guardian_; }
    const HeroStatus& hero_status() const { return status_; }
    const std::vector<WorldEffect>& effects() const { return effects_; }
    const std::vector<Aura>& auras() const { return auras_; }
    // ao.a(ad) type 10 only marks cf 7 expired (cf.b); the next ao.d still ticks it once
    // (a drain on a multiple of 10) and then removes it with its icon.
    bool cure_poison() { const bool had = status_.poison > 0; if (had) status_.poison = 1; return had; }
    // Remaining cooldown of a guardian skill (p.c), for the HUD.
    int guardian_cooldown(int id, int skill) const;
    static int guardian_cooldown_full(int id, int skill); // p.b[id*3+skill]
    // ae constructor: create the boss encounter of maps 11/13/15/82 (no-op elsewhere).
    void spawn_bosses(int map_id);
    bool boss_encounter() const;
    std::pair<int, int> screen_shake() const { return {shake_x_, shake_y_}; }
    void clear_shake() { shake_x_ = shake_y_ = 0; }

    // Exposed for tests.
    int rand_range(int lo, int hi); // h.a(int,int), inclusive
    int next_int();                 // ck.a.nextInt()
    int roll_hit_option(const HeroCombatView& hero);
    void hit_enemy(std::size_t index, int damage, bool knock, int direction, bool critical,
                   int knock_power, int status, HeroCombatView& hero,int hit_kind=1);
    // gone_stats >= 0: the firing al no longer occupies enemy_index (its slot was reused); the
    // hit uses its stats and skips targeting/reflect on the new occupant.
    void hit_hero(std::size_t enemy_index, int damage, int direction, HeroCombatView& hero, int gone_stats = -1);

private:
    struct Spawn { int delay{}, sprite{}, stats{}, x{}, y{}; };
    const EnemyStats& stat(const Enemy& e) const { return (*stats_)[static_cast<std::size_t>(e.stats)]; }
    bool in_bounds(int x, int y) const { return x >= 0 && y >= 0 && x < width_ && y < height_; }
    bool tile_free(int x, int y) const; // ae.a(int,int)
    bool can_place(int self, int width, int x, int y) const; // ae.a(o,int,int)
    bool can_step(int self, int direction) const;
    int occupant(int x, int y) const;  // 0 empty, -1 hero, -2 npc, >0 enemy index+1
    int occupant_at(const Enemy& e, int direction, int distance) const; // ck.a(byte,byte)
    bool melee_reaches_hero(std::size_t index) const;                    // o.a(byte,ck) == hero
    void rebuild_occupancy(const HeroCombatView& hero);
    void unregister(int index);
    void register_actor(int index);
    void move_enemy(Enemy& e, int index, int pixels);
    bool spawn_enemy(int x, int y, int sprite, int stats_index, bool scatter, int radius, int attempts);
    void update_spawns(bool scatter = true);
    void place_initial_spawns(const HeroCombatView& hero);
    void update_enemy(std::size_t index, HeroCombatView& hero, CombatEvents& events);
    void enemy_think(std::size_t index, HeroCombatView& hero);
    void enemy_walk_boundary(std::size_t index, HeroCombatView& hero);
    void enemy_rest(Enemy& e, bool randomize);
    void enemy_start_attack(Enemy& e);
    void enemy_wander(Enemy& e);
    void enemy_attack_frame(std::size_t index, HeroCombatView& hero);
    void enemy_chase(std::size_t index, int range, int target_x, int target_y, bool hero_target);
    int chase_side(std::size_t index, int direction, bool alt);
    void enemy_die(std::size_t index, HeroCombatView& hero, CombatEvents& events);
    void update_projectiles(HeroCombatView& hero);
    void update_hero_attack(HeroCombatView& hero, CombatEvents& events);
    void hero_hit_frame(HeroCombatView& hero);
    bool hero_class6_hit(HeroCombatView& hero);
    bool hero_class7_hit(HeroCombatView& hero);
    bool hero_class8_hit(HeroCombatView& hero);
    void hero_strike(int target, int knock, int direction, int power, HeroCombatView& hero);
    int enemy_in_front(const HeroCombatView& hero, int direction, int distance) const;
    void end_combo(int index);
    void add_popup(PopupKind kind, int value, int x_px, int y_px, int target = -2);
    void hero_damaged(HeroCombatView& hero, int amount);
    // Bosses (combat_boss.cpp).
    int add_boss(BossKind kind, int x, int y, int stats_index, int width, int state, int direction);
    void update_boss(std::size_t index, HeroCombatView& hero, CombatEvents& events);
    void boss_think(std::size_t index, HeroCombatView& hero);
    void boss_attack_frame(std::size_t index, HeroCombatView& hero, CombatEvents& events);
    void boss_finish(std::size_t index, CombatEvents& events);
    void boss_death(std::size_t index);
    void hit_boss(std::size_t index, int damage, bool critical, HeroCombatView& hero,int hit_kind,int option);
    void cc_teleport_near(std::size_t index, HeroCombatView& hero);
    void cc_set_pattern(Enemy& e, int pattern);
    void boss_projectile(std::size_t index, int x, int y, int direction, int range);
    // aw(9,-1,n) on an actor (target -1 hero, >=0 enemy) or y(BB[B) at a tile (target -2).
    void eif_effect(int sprite, int target, int x_px = 0, int y_px = 0);
    void remove_enemy(std::size_t index);
    bool in_area(const HeroCombatView& hero, int x0, int x1, int y0, int y1) const;
    void heal_enemy(std::size_t source, std::size_t target);
    // Guardian (combat_guardian.cpp).
    void update_guardian(HeroCombatView& hero, CombatEvents& events);
    void guardian_effect(HeroCombatView& hero, CombatEvents& events);
    void guardian_strike(int x, int y, int damage);
    void guardian_strike_dir(int direction, int distance, int damage);
    void guardian_wave(int start, int step, int damage);
    void guardian_line(const std::array<int, 4>& ticks, int damage);
    void hit_enemy_element(std::size_t index, int damage, int element);
    int guardian_element() const;
    void ensure_cooldowns(const HeroCombatView& hero);
    void update_status(HeroCombatView& hero);
    void age_attached();
public:
    // ah.a state 1 -> n.n during scripted movement/delays: ao.d runs the cf timers (regen
    // needs n.e == 2) and p.e the guardian; ordinary enemies (ae.b) stay frozen.
    void event_tick(HeroCombatView& hero, CombatEvents& events) {
        place_initial_spawns(hero); // ae.d ran at load, before any event tick
        age_attached(); // aw.draw ages owner effects whenever the world is drawn
        if (!hero_dying_) update_status(hero);
        age_effects();
        update_guardian(hero, events);
    }
private:
    void spawn_effect(int kind, int x_px, int y_px);
    void spawn_effect_at(int kind, int distance, int side); // relative to the guardian tile
    void add_aura(int target, int delay, int duration);
    void age_effects();
    void select_target(std::size_t index,bool keep=false) {
        if(!keep || !target_enemy()) target_=int(index);
        target_ticks_=24;
    }
    void hit_effect(int kind,int target,int x,int y) {battle_effects_.push_back({kind,target,x,y,0,3});}

    int width_{}, height_{};
    std::vector<std::int8_t> overlay_;
    std::vector<std::int16_t> occupancy_;
    std::vector<std::pair<int, int>> npc_tiles_;
    const std::vector<EnemyStats>* stats_{};
    std::vector<EnemyFrames> frames_ = std::vector<EnemyFrames>(64);
    std::vector<Enemy> enemies_;
    std::vector<Spawn> spawns_;
    int spawn_timer_{};
    bool initial_spawns_{}; // ae.d still pending for the freshly loaded map
    std::vector<GroundDrop> drops_;
    std::vector<Popup> popups_;
    std::vector<int> sounds_;
    std::vector<Projectile> projectiles_;
    std::uint64_t rng_{};
    int hero_x_{}, hero_y_{};
    int shake_x_{}, shake_y_{};
    FieldGuardian guardian_;
    HeroStatus status_;
    std::array<std::array<int, 3>, 6> guardian_cooldowns_{};
    std::array<bool, 6> guardian_seen_{};
    std::vector<WorldEffect> effects_;
    std::vector<Aura> auras_;
    std::vector<BattleEffect> battle_effects_;
    std::array<int,3> death_effect_frames_{{1,1,1}};
    std::array<int,4> visible_{};
    int next_generation_{};
    bool has_visible_{};
    int target_{-1},target_ticks_{};
    int hero_render_frame_{},hero_shake_{},hero_shake_direction_{2};

    // Hero attack state (ao fields).
    int hero_state_{1};
    int hero_frame_{};
    std::array<int, 5> combo_{};
    int combo_index_{-1};
    int max_combo_{1};
    int recover_{};
    int dash_{};
    bool hero_dying_{};
    int death_timer_{};
    int strike_damage_{}, strike_status_{-1};
    bool strike_critical_{};
    bool hero_dashing_{};
    int hp_regen_timer_{68}, sp_regen_timer_{21}; // ao.v / ao.w
};

// ao.n: derived attack/defense contributions the port does not track elsewhere.
inline int hero_base_attack(int power) { return power * 4 / 5; }
inline int hero_base_defense(int power, int level) { return power / 5 + level / 3; }

} // namespace combat
