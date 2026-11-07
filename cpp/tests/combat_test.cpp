#include <algorithm>
#include "combat.hpp"
#include "map.hpp"

#include <iostream>
#include <stdexcept>

using namespace combat;

static void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

static EnemyStats dummy_stats() {
    EnemyStats s;
    s.level = 5; s.hp = 30; s.attack = 20; s.defense = 2; s.evasion = -100;
    s.aggro_range = 5; s.move_delay = 3; s.attack_delay = 2; s.respawn_delay = 32;
    return s;
}

static HeroCombatView hero_at(int x, int y, int direction) {
    HeroCombatView h;
    h.x_tile = x; h.y_tile = y; h.direction = direction;
    h.hp = h.max_hp = 100; h.sp = h.max_sp = 100;
    h.attack = 40; h.defense = 5; h.agility = 0; h.level = 1;
    return h;
}

static void synthetic() {
    std::vector<EnemyStats> stats{dummy_stats()};
    CombatWorld world;
    world.reset(10, 10, std::vector<std::int8_t>(100, 0), &stats);
    world.set_enemy_frames(0, {2, 2, 4});
    world.queue_spawn(0, 0, 5, 5);
    auto hero = hero_at(5, 7, 1);
    CombatEvents events;
    world.tick(hero, events); // spawn timer fires on the first tick
    check(world.enemies().size() == 1, "queued enemy spawns");
    const auto& e = world.enemies()[0];
    check(e.hp == 30 && e.state == EnemyState::idle, "spawn uses table HP");

    // Enemy approaches; within a few dozen ticks it reaches melee and hits the hero.
    for (int i = 0; i < 200 && hero.hp == 100; ++i) world.tick(hero, events);
    check(hero.hp < 100, "enemy reaches and hits the hero");
    check(world.enemies()[0].target == 1, "enemy targets hero after aggro");

    // Direct damage: 40 - defense 2 = 38 >= 30 HP, enemy dies, then gives EXP after the death timer.
    world.hit_enemy(0, 40, false, 2, false, 1, -1, hero);
    check(world.enemies()[0].state == EnemyState::dying, "lethal hit starts dying state");
    for (int i = 0; i < 6; ++i) world.tick(hero, events);
    check(world.enemies()[0].state == EnemyState::dead, "dying enemy is removed");
    // level 5 vs hero 1: factor min(20-(1-5),26)=24 -> 5*24/2 = 60.
    check(events.experience == 60, "kill experience follows al.l formula");
    // Respawn after 32 ticks (two 16-tick spawn periods).
    bool respawned = false;
    for (int i = 0; i < 80 && !respawned; ++i) {
        world.tick(hero, events);
        respawned = world.enemies()[0].state != EnemyState::dead;
    }
    check(respawned, "respawn queue recreates the enemy");
}

static void hero_attack() {
    auto stats = std::vector<EnemyStats>{dummy_stats()};
    stats[0].hp = 1000; stats[0].aggro_range = 0; stats[0].move_delay = 100;
    for (int cls = 6; cls <= 8; ++cls) {
        CombatWorld world;
        world.reset(10, 10, std::vector<std::int8_t>(100, 0), &stats);
        check(world.place_enemy(0, 0, 4, 3), "target placed");
        auto hero = hero_at(4, 4, 1);
        hero.character_class = cls;
        CombatEvents events;
        hero.combat_map = false;
        check(!world.request_attack(hero, false) && !world.hero_attacking(), "ae.c: peaceful maps reject attacks");
        hero.combat_map = true;
        check(world.request_attack(hero, false), "first attack accepted");
        check(world.hero_attacking() && world.hero_sprite_state() == 3, "combo step 0 uses sprite state 3");
        for (int i = 0; i < 6; ++i) world.tick(hero, events);
        check(!world.hero_attacking(), "single attack ends after its frame count");
        check(world.enemies()[0].hp < 1000, "front hit lands");
        check(hero.sp < 100, "attack step spends SP");
    }
}

static void combo_length() {
    std::vector<EnemyStats> stats{dummy_stats()};
    CombatWorld world;
    world.reset(6, 6, std::vector<std::int8_t>(36, 0), &stats);
    auto hero = hero_at(2, 3, 1);
    check(world.request_attack(hero, false), "first step");
    {
        CombatEvents events;
        world.tick(hero, events);
    }
    check(!world.request_attack(hero, false), "ao.z = 1 rejects a queued second step");
    world.clear_hero();
    hero.max_combo = 3;
    check(world.request_attack(hero, false), "start");
    CombatEvents events;
    world.tick(hero, events); // q: -1 -> 0
    check(world.request_attack(hero, false), "COMBO raises the queue length");
    for (int i = 0; i < 4; ++i) world.tick(hero, events);
    check(world.hero_sprite_state() == 4, "second combo step uses sprite state 4");
}

static void guardian() {
    std::vector<EnemyStats> stats{dummy_stats()};
    stats[0].hp = 10000; stats[0].aggro_range = 0; stats[0].move_delay = 100;
    CombatWorld world;
    world.reset(12, 12, std::vector<std::int8_t>(144, 0), &stats);
    auto hero = hero_at(5, 9, 1);
    hero.guardian_id = 0; hero.guardian_level = 5; hero.guardian_calm = 0; hero.guardian_wild = -1;
    world.place_enemy(0, 0, 5, 6); // two tiles ahead of the guardian's tile (5,8)
    check(!world.summon_guardian(hero, true), "assigned skill starts on cooldown");
    CombatEvents events;
    for (int i = 0; i < 56; ++i) world.tick(hero, events);
    check(world.guardian_cooldown(0, 0) == 0, "cooldown p.b = 56 ticks");
    hero.guardian_wild = 2;
    check(!world.summon_guardian(hero, false) && world.guardian_cooldown(0, 2) > 0, "p(): unassigned skills start on cooldown too");
    hero.guardian_wild = -1;
    check(!world.summon_guardian(hero, false), "locked wild slot");
    world.hit_enemy(0, 1, false, 1, false, 0, 0, hero); // cf Sleep
    check(world.enemies()[0].status_elapsed[0] >= 0, "enemy asleep before the summon");
    const int slept_hp = world.enemies()[0].hp;
    hero.will = 1000; // gear bonus: guardian formulas use base Will only
    check(world.summon_guardian(hero, true), "calm summon");
    check(world.guardian().x == 5 && world.guardian().y == 8 && world.guardian().state == 1, "guardian appears ahead");
    for (int i = 0; i < 40 && world.enemies()[0].hp == slept_hp; ++i) world.tick(hero, events);
    check(world.enemies()[0].hp < slept_hp && world.enemies()[0].status_elapsed[0] == -1, "al.t: guardian element hit wakes Sleep");
    check(slept_hp - world.enemies()[0].hp < 500, "guardian damage ignores the Will gear bonus");
    for (int i = 0; i < 40; ++i) world.tick(hero, events);
    // p.g: the line strike reaches 0..3 tiles ahead of the guardian, twice.
    check(world.enemies()[0].hp < 10000, "guardian line strike damages the enemy");
    check(world.guardian().state == 0, "guardian leaves after its effect");
    check(world.guardian_cooldown(0, 0) > 0, "skill back on cooldown");

    // Guardian 1 skill 0 heals 30% of max HP.
    CombatWorld healer;
    healer.reset(12, 12, std::vector<std::int8_t>(144, 0), &stats);
    auto hurt = hero_at(5, 9, 1);
    hurt.hp = 10; hurt.guardian_id = 1; hurt.guardian_calm = 0;
    for (int i = 0; i < 300 && !healer.summon_guardian(hurt, true); ++i) healer.tick(hurt, events);
    for (int i = 0; i < 20; ++i) healer.tick(hurt, events);
    check(hurt.hp >= 40, "healing skill restores 30%");
}

static void summon_reallocation() {
    std::vector<EnemyStats> stats{dummy_stats()};
    stats[0].summoner=true;stats[0].summon_element=1;stats[0].hp=10000;
    CombatWorld world;world.reset(32,32,std::vector<std::int8_t>(1024,0),&stats,123);
    world.place_enemy(0,0,5,5);
    auto hero=hero_at(20,20,1);hero.agility=1000;
    world.hit_enemy(0,0,false,1,false,0,-1,hero);
    for(int i=0;i<42;++i) {CombatEvents events;world.tick(hero,events);}
    check(world.enemies().size()==2,"summoner can grow enemy storage safely");
    check(world.enemies()[0].summon_timer==-10,"summoner timer survives vector growth");
}

static void cancelled_guardian() {
    std::vector<EnemyStats> stats{dummy_stats()};
    for(int id:{0,1,4}) {
        CombatWorld world;world.reset(32,32,std::vector<std::int8_t>(1024,0),&stats);
        auto hero=hero_at(10,10,1);hero.guardian_id=id;hero.guardian_calm=id==4?1:2;
        bool summoned=false;
        for(int i=0;i<600 && !summoned;++i) {
            summoned=world.summon_guardian(hero,true);
            if(!summoned) {CombatEvents events;world.tick(hero,events);}
        }
        check(summoned,"protective guardian summon available");
        const auto active=[&]() {const auto& s=world.hero_status();return s.thorns || s.regen_double || s.invulnerable;};
        for(int i=0;i<100 && !active();++i) {CombatEvents events;world.tick(hero,events);}
        check(active(),"guardian applies protection before transition");
        const int cooldown=world.guardian_cooldown(id,hero.guardian_calm);
        world.reset(32,32,std::vector<std::int8_t>(1024,0),&stats);
        check(!active() && world.guardian().state==0 && world.auras().empty(),"map change cancels summon-owned protection");
        check(world.guardian_cooldown(id,hero.guardian_calm)==cooldown,"map change retains existing cooldown");
        for(int i=0;i<400;++i) {CombatEvents events;world.tick(hero,events);}
        check(!active(),"cancelled guardian cannot leave permanent protection");
        world.reset_hero_state();
        check(world.guardian_cooldown(id,hero.guardian_calm)==0,"load/new game clears prior cooldown state");
        CombatEvents events;world.tick(hero,events);
        check(world.guardian_cooldown(id,hero.guardian_calm)>0,"new hero initializes skill cooldown independently");
    }
}

static void guardian_attack_elements() {
    for(int guardian=-1;guardian<6;++guardian) for(int element=1;element<=3;++element) {
        std::vector<EnemyStats> stats(43,dummy_stats());stats[0].hp=10000;stats[0].element=element;
        stats[0].summoner=true;stats[0].summon_element=element;
        CombatWorld world;world.reset(12,12,std::vector<std::int8_t>(144,0),&stats);
        world.place_enemy(0,0,5,5);
        auto hero=hero_at(10,10,1);hero.agility=1000;hero.guardian_id=guardian;
        world.hit_enemy(0,102,false,1,false,0,-1,hero);
        const int source=guardian<0?0:guardian%3+1;
        check(world.enemies()[0].hp==10000-100*kElement[source][element]/10,"normal hit uses equipped guardian element after defense");
        check(world.enemies()[0].summon_timer==(source==element?-10:40),"summoner compares its trigger element with equipped guardian");
        stats[35]=stats[0];
        world.reset(16,16,std::vector<std::int8_t>(256,0),&stats);world.spawn_bosses(13);
        world.hit_enemy(0,102,false,1,false,0,-1,hero);
        check(world.enemies()[0].hp==10000-100*kElement[source][element]/10,"boss hit uses the same guardian element after defense");
    }
}

static void critical_weapon_rules() {
    std::vector<EnemyStats> stats(43,dummy_stats());
    for(auto& stat:stats) {stat.hp=10000;stat.element=2;}
    auto hero=hero_at(10,10,1);hero.agility=1000;hero.will=27;
    hero.guardian_id=0;hero.weapon_critical_chance=7;hero.weapon_critical_bonus_tenths=3;
    check(hero_critical_chance(hero)==342,"critical chance includes weapon bonus with original integer divisions");
    for(bool boss:{false,true}) for(bool critical:{false,true}) {
        CombatWorld world;world.reset(16,16,std::vector<std::int8_t>(256,0),&stats);
        if(boss) world.spawn_bosses(13);else world.place_enemy(0,0,5,5);
        world.hit_enemy(0,103,false,1,critical,0,-1,hero);
        // (103-2)*6/10 =60; critical adds60*3/10 =18, not a bonus before defense/element.
        check(world.enemies()[0].hp==10000-(critical?78:60),"critical bonus applies after defense and element for al/av");
    }
}

static void battle_presentation() {
    const auto large=damage_number_frame(123,0),small=damage_number_frame(123,2);
    check(large.strip==7 && large.width==9 && large.height==14 && large.advance==8,"original early damage uses large digit strip");
    check(small.strip==5 && small.width==7 && small.height==9 && small.advance==6,"original late damage uses small digit strip");
    check(damage_number_frame(-20,1).strip==8 && damage_number_frame(-20,3).strip==6,"hero damage uses red strips at both sizes");
    std::vector<EnemyStats> stats{dummy_stats()};
    stats[0].hp=10000;stats[0].move_delay=1000;stats[0].attack_delay=1000;stats[0].aggro_range=0;stats[0].size=1;
    CombatWorld world;world.reset(32,32,std::vector<std::int8_t>(1024,0),&stats);
    world.set_death_effect_frames(1,5);
    world.place_enemy(0,0,5,5);
    auto hero=hero_at(20,20,1);hero.agility=1000;
    world.hit_enemy(0,100,false,4,false,0,-1,hero);
    check(world.target_enemy() && world.target_enemy()->hp<10000,"hit selects current enemy HP");
    check(!world.battle_effects().empty() && world.battle_effects().back().frame==0,"hit spark starts at frame zero");
    check(world.popups().back().age==0,"damage starts with large frame-zero digits");
    for(int i=0;i<24;++i) {CombatEvents events;world.tick(hero,events);}
    check(!world.target_enemy() && world.battle_effects().empty() && world.popups().empty(),"HP panel expires after 24 ticks and effects age out");
    world.hit_enemy(0,20000,false,4,false,0,-1,hero);
    for(int i=0;i<4;++i) {CombatEvents events;world.tick(hero,events);}
    check(world.enemies()[0].state==EnemyState::dead && !world.target_enemy(),"dead target leaves HP panel");
    check(!world.battle_effects().empty() && world.battle_effects().back().kind==101 && world.battle_effects().back().frame==0,"death uses size-specific EIF at first frame");
    for(int i=0;i<5;++i) {CombatEvents events;world.tick(hero,events);}
    check(world.battle_effects().empty(),"death effect respects loaded EIF duration");
    world.reset(32,32,std::vector<std::int8_t>(1024,0),&stats);
    hero.hp=hero.max_hp;hero.sp=hero.max_sp;
    world.request_attack(hero,false);CombatEvents events;world.tick(hero,events);
    check(world.hero_frame()==0,"first attack frame is presented rather than skipped");
    world.tick(hero,events);check(world.hero_frame()==1,"attack presentation advances on game ticks");
    world.reset(32,32,std::vector<std::int8_t>(1024,0),&stats);world.place_enemy(0,0,5,5);
    hero.agility=-1000;hero.defense=0;
    for(int i=0;i<20 && world.hero_hit_offset()==std::pair<int,int>{0,0};++i) world.hit_hero(0,5,4,hero);
    check(world.hero_hit_offset()==std::pair<int,int>{2,0},"hero hit shakes two pixels in hit direction");
    world.tick(hero,events);check(world.hero_hit_offset()==std::pair<int,int>{0,0},"hit shake clears on next game tick");
}

static void poison() {
    std::vector<EnemyStats> stats{dummy_stats()};
    stats[0].attack_kind = 1; stats[0].evasion = 1000;
    CombatWorld world;
    world.reset(6, 6, std::vector<std::int8_t>(36, 0), &stats);
    world.place_enemy(0, 0, 2, 2);
    auto hero = hero_at(2, 3, 1);
    hero.agility = -1000; hero.defense = 1000; hero.max_hp = hero.hp = 1000;
    for (int i = 0; i < 200 && world.hero_status().poison == 0; ++i) world.hit_hero(0, 1, 2, hero);
    check(world.hero_status().poison == 80, "melee kind 1 poisons (cf 7, 80 ticks)");
    check(world.hero_status().order==std::vector<int>{7},"poison creates one hero status icon");
    CombatEvents events;
    for (int i = 0; i < 10; ++i) world.tick(hero, events);
    check(hero.hp < 1000, "poison drains max HP / 25 every 10 ticks");
    check(hero.hp==960 && !world.popups().empty() && world.popups().back().value==-83,
          "original poison subtracts maxHP/25 but displays maxHP/12");
    world.reset(6,6,std::vector<std::int8_t>(36,0),&stats);
    check(world.hero_status().poison>0,"ordinary map change keeps timed poison");
    check(world.cure_poison() && world.hero_status().poison == 1, "antidote expires poison (cf.b)");
    check(world.hero_status().order==std::vector<int>{7},"expired poison stays until the next ao.d");
    world.tick(hero, events);
    check(world.hero_status().poison == 0 && world.hero_status().order.empty(),"next tick removes cured poison");
    world.place_enemy(0,0,2,2);
    for(int i=0;i<200 && world.hero_status().poison==0;++i) world.hit_hero(0,1,2,hero);
    check(world.hero_status().poison>0,"poison active before hero replacement");
    world.reset_hero_state();
    check(world.hero_status().poison==0,"loaded/new hero cannot inherit poison");
}

static void hero_death() {
    std::vector<EnemyStats> stats{dummy_stats()};
    stats[0].attack = 500;
    CombatWorld world;
    world.reset(6, 6, std::vector<std::int8_t>(36, 0), &stats);
    auto hero = hero_at(2, 3, 1);
    hero.agility = -1000; // never evade
    world.place_enemy(0, 0, 2, 2);
    CombatEvents events;
    world.hit_hero(0, 500, 2, hero);
    check(hero.hp == 0 && world.hero_dying(), "lethal damage starts hero death");
    for (int i = 0; i < 30 && !events.hero_died; ++i) world.tick(hero, events);
    check(events.hero_died, "death timer of 24 ticks reports game over");
}

static void guardian_summon_while_dying() {
    std::vector<EnemyStats> stats{dummy_stats()};
    stats[0].aggro_range = 0; stats[0].move_delay = 100;
    CombatWorld world;
    world.reset(8, 8, std::vector<std::int8_t>(64, 0), &stats);
    auto hero = hero_at(4, 5, 1);
    hero.agility = -1000;
    hero.guardian_id = 0; hero.guardian_level = 1; hero.guardian_calm = 0; hero.guardian_wild = -1;
    world.place_enemy(0, 0, 1, 1);
    CombatEvents events;
    for (int i = 0; i < 56; ++i) world.tick(hero, events);
    world.hit_hero(0, 5000, 2, hero);
    check(world.hero_dying() && world.guardian_cooldown(0, 0) == 0, "hero dying with the calm skill ready");
    check(world.summon_guardian(hero, true), "ao.a(boolean): no o.h check, the dying hero can still summon");
}

static void pickup() {
    std::vector<EnemyStats> stats{dummy_stats()};
    stats[0].drops = {{5, 1, 150}};
    stats[0].hp = 1;
    CombatWorld world;
    world.reset(6, 6, std::vector<std::int8_t>(36, 0), &stats);
    auto hero = hero_at(1, 1, 2);
    world.place_enemy(0, 0, 3, 3);
    CombatEvents events;
    const int ex = world.enemies()[0].tile_x, ey = world.enemies()[0].tile_y;
    world.hit_enemy(0, 100, false, 2, false, 1, -1, hero);
    for (int i = 0; i < 6; ++i) world.tick(hero, events);
    check(!world.drops().empty(), "drop table chance 150 always drops");
    hero.x_tile = ex; hero.y_tile = ey;
    const std::size_t lying = world.drops().size();
    {   // D01: a refused item stays and the search goes on (ae.a(BB)[B).
        CombatEvents refused;
        const bool took = world.try_pickup(hero, refused, [](int, int) { return false; });
        check(!took ? (refused.messages.size() == 1 && world.drops().size() == lying)
                    : (refused.gold > 0 && refused.items.empty()), "refused item does not block the rest of the tile");
    }
    while (world.try_pickup(hero, events, [](int, int) { return true; })) {}
    check(events.items.size() >= 1 && events.items[0].first == 5 && events.items[0].second == 1, "item pickup");
}

// Hits the given enemy until it dies, ticking the world; returns accumulated events.
static CombatEvents slay(CombatWorld& world, std::size_t index, HeroCombatView& hero, int ticks = 400) {
    CombatEvents all;
    hero.agility = 1000; // never miss, so the test is deterministic
    for (int i = 0; i < ticks; ++i) {
        if (world.enemies()[index].state != EnemyState::dying && world.enemies()[index].state != EnemyState::dead)
            world.hit_enemy(index, 5000, false, 2, false, 1, -1, hero);
        hero.hp = hero.max_hp;
        CombatEvents events;
        world.tick(hero, events);
        all.start_trigger = std::max(all.start_trigger, events.start_trigger);
        all.overlay_writes.insert(all.overlay_writes.end(), events.overlay_writes.begin(), events.overlay_writes.end());
        if (world.enemies()[index].state == EnemyState::dead || all.start_trigger || !all.overlay_writes.empty()) break;
    }
    return all;
}

static void bosses(const std::vector<EnemyStats>& stats) {
    auto hero = hero_at(2, 14, 1);
    hero.max_hp = hero.hp = 100000;
    {   // Map 11: cc, death starts trigger 1 and grants no EXP (av.l replaces al.l).
        CombatWorld world;
        world.reset(20, 16, std::vector<std::int8_t>(320, 0), &stats);
        world.spawn_bosses(11);
        check(world.enemies().size() == 1 && world.enemies()[0].boss == BossKind::cc &&
              world.enemies()[0].tile_x == 10 && world.enemies()[0].tile_y == 9, "cc spawn");
        CombatEvents events;
        for (int i = 0; i < 60; ++i) world.tick(hero, events);
        check(world.boss_encounter(), "cc keeps fighting");
        const auto result = slay(world, 0, hero);
        check(result.start_trigger == 1 && !world.boss_encounter(), "cc death starts trigger 1");
        check(events.experience == 0, "boss kills give no field EXP");
    }
    {   // Map 13: ar, then the ag/bd/cd trio; ag's death removes its partners and ends the fight.
        CombatWorld world;
        world.reset(20, 16, std::vector<std::int8_t>(320, 0), &stats);
        world.spawn_bosses(13);
        check(world.enemies().size() == 1 && world.enemies()[0].boss == BossKind::ar, "ar spawn");
        auto result = slay(world, 0, hero);
        check(result.start_trigger == 0, "ar death is only the first phase");
        CombatEvents events;
        world.tick(hero, events);
        check(world.enemies().size() == 4 && world.enemies()[1].boss == BossKind::ag &&
              world.enemies()[2].boss == BossKind::bd && world.enemies()[3].boss == BossKind::cd, "trio spawns");
        for (int i = 0; i < 80; ++i) world.tick(hero, events);
        result = slay(world, 1, hero);
        check(result.start_trigger == 1, "ag death ends the encounter");
        check(world.enemies()[2].state == EnemyState::dead && world.enemies()[3].state == EnemyState::dead,
              "partners vanish with ag");
    }
    {   // Map 15: cg with ghost hands; the corpse turns rows 7..9 of columns 6..9 into trigger 1.
        CombatWorld world;
        world.reset(16, 16, std::vector<std::int8_t>(256, 0), &stats);
        world.spawn_bosses(15);
        check(world.enemies().size() == 3 && world.enemies()[2].boss == BossKind::cg, "cg spawn");
        check(!world.occupied_by_enemy(0, 12) && !world.occupied_by_enemy(13, 11), "hands never block tiles");
        auto low = hero_at(8, 13, 1);
        low.max_hp = low.hp = 100000;
        CombatEvents events;
        for (int i = 0; i < 120; ++i) { low.hp = low.max_hp; world.tick(low, events); }
        const auto result = slay(world, 2, low);
        check(result.overlay_writes.size() == 12 && result.overlay_writes.front().value == 1, "cg corpse trigger tiles");
        check(world.enemies()[0].state == EnemyState::dead && world.enemies()[1].state == EnemyState::dead,
              "hands removed with cg");
    }
    {   // Map 82: bv never attacks.
        CombatWorld world;
        world.reset(16, 16, std::vector<std::int8_t>(256, 0), &stats);
        world.spawn_bosses(82);
        auto near = hero_at(7, 11, 1);
        CombatEvents events;
        for (int i = 0; i < 200; ++i) world.tick(near, events);
        check(near.hp == near.max_hp, "bv stays passive");
        check(slay(world, 0, near).start_trigger == 1, "bv death starts trigger 1");
    }
}

static void hit_options() {
    std::vector<EnemyStats> stats(43,dummy_stats());
    for(auto& s:stats) {s.hp=1000;s.element=0;s.aggro_range=0;}
    for(bool boss:{false,true}) for(int option=0;option<9;++option) {
        CombatWorld world;world.reset(32,32,std::vector<std::int8_t>(1024,0),&stats);
        if(boss) world.spawn_bosses(13); else world.place_enemy(0,0,5,5);
        auto hero=hero_at(25,25,1);hero.hp=hero.sp=1;hero.guardian_id=0;
        world.hit_enemy(0,102,false,1,false,0,option,hero);
        const auto& e=world.enemies()[0];
        check(e.hp==1000-((option==2&&!boss)?1000:option==8?200:100),"option damage and boss Deadly immunity");
        check(hero.sp==(option==3?(boss?31:81):1),"Focused restores original normal/boss SP fraction");
        check(hero.hp==(option==4?51:1),"Absorption restores half damage");
        for(int s=0;s<5;++s) check((e.status_elapsed[s]>=0)==(!boss&&kOptionStatus[option]==s),"only normal enemies receive cf statuses");
    }
    CombatWorld world;world.reset(32,32,std::vector<std::int8_t>(1024,0),&stats);
    world.place_enemy(0,0,5,5);auto hero=hero_at(25,25,1);CombatEvents events;
    world.hit_enemy(0,2,false,1,false,0,6,hero);
    for(int i=0;i<8;++i) world.tick(hero,events);
    check(world.enemies()[0].hp>=975&&world.enemies()[0].hp<=985,"offscreen bleeding ticks after eight frames");
    world.hit_enemy(0,2,false,1,false,0,6,hero);
    check(world.enemies()[0].status_elapsed[3]==0,"reapplication refreshes status");
    check(world.enemies()[0].status_order==std::vector<int>{3},"refresh keeps one icon in its insertion slot");
    for(int i=0;i<40;++i) world.tick(hero,events);
    check(world.enemies()[0].status_elapsed[3]==-1,"status expires at forty frames");
    check(world.enemies()[0].status_order.empty(),"expired status removes its icon");
    world.hit_enemy(0,2,false,1,false,0,0,hero);
    world.hit_enemy(0,2,false,1,false,0,-1,hero);
    check(world.enemies()[0].status_elapsed[0]==-1,"next hit wakes Sleep");
    check(world.enemies()[0].status_order.empty(),"waking removes Sleep icon");
    for(int option:{1,5,6,7,0}) world.hit_enemy(0,2,false,1,false,0,option,hero);
    check(world.enemies()[0].status_order==std::vector<int>({1,4,3,2,0}),"status icons preserve application order");
    hero.weapon_hit_option=0;hero.shield_hit_option=8;
    int weapon=0,shield=0,none=0;
    for(int i=0;i<10000;++i) {const int v=world.roll_hit_option(hero);weapon+=v==0;shield+=v==8;none+=v==-1;}
    check(weapon>1800&&weapon<2200&&shield>650&&shield<950&&none>6800,"weapon priority and shield fallback probabilities");
    int rage_procs=0;
    for(int seed=1;seed<=100;++seed) {
        CombatWorld strike;strike.reset(10,10,std::vector<std::int8_t>(100,0),&stats,seed);
        strike.place_enemy(0,0,4,3);
        auto attacker=hero_at(4,4,1);attacker.weapon_hit_option=8;
        strike.request_attack(attacker,false);
        for(int i=0;i<3;++i) strike.tick(attacker,events);
        if(attacker.hp<100) {
            check(attacker.hp==96,"Rage charges max HP /25 once per strike");++rage_procs;
            bool icon=false,red_cost=false;
            for(const auto& effect:strike.battle_effects()) if(effect.kind==200&&effect.duration==8) icon=true;
            for(const auto& popup:strike.popups()) if(popup.value==-4) red_cost=true;
            check(icon&&red_cost,"Rage has eight-frame icon and red cost popup");
        }
    }
    check(rage_procs>0,"seeded attacks exercise Rage self damage");
}

// Audit (2026-11-02): projectile order, guardian lifecycle, enemy targeting and visibility.
static void audit_regressions() {
    const auto summon=[](CombatWorld& world,HeroCombatView& hero) {
        for(int i=0;i<1200;++i) {if(world.summon_guardian(hero,true)) return true;CombatEvents events;world.tick(hero,events);}
        return false;
    };
    {   // S04: a scripted collision change blocks the enemy's path; S02: event ticks age aw popups.
        std::vector<EnemyStats> stats{dummy_stats()};
        stats[0].move_delay=1;stats[0].hp=1000;stats[0].attack_kind=0;
        CombatWorld world;world.reset(20,20,std::vector<std::int8_t>(400,0),&stats);
        world.set_enemy_frames(0,{2,2,4});
        world.place_enemy(0,0,5,5);
        world.set_collision(6,5,std::int8_t(-128));
        auto hero=hero_at(7,5,3);hero.hp=hero.max_hp=10000;
        for(int i=0;i<12;++i) {CombatEvents events;world.tick(hero,events);}
        check(!(world.enemies()[0].tile_x==6 && world.enemies()[0].tile_y==5),"enemy respects scripted collision");
        world.hit_enemy(0,10,false,1,false,1,-1,hero);
        check(!world.popups().empty(),"hit leaves a popup");
        for(int i=0;i<10;++i) {CombatEvents events;world.event_tick(hero,events);}
        check(world.popups().empty(),"event ticks expire attached popups");
    }
    for (const auto& spot : {std::pair{5,6}, std::pair{6,6}, std::pair{7,5}}) { // E03: melee over the whole footprint
        std::vector<EnemyStats> stats{dummy_stats()};
        stats[0].size=2;stats[0].attack_kind=0;stats[0].hp=10000;stats[0].attack=200;
        CombatWorld world;world.reset(20,20,std::vector<std::int8_t>(400,0),&stats);
        world.set_enemy_frames(0,{2,2,4});
        world.place_enemy(0,0,5,5);
        auto hero=hero_at(spot.first,spot.second,1);hero.hp=hero.max_hp=10000;hero.defense=0;hero.agility=-100;
        for(int i=0;i<60;++i) {CombatEvents events;world.tick(hero,events);}
        check(hero.hp<10000,"two-tile enemy reaches the hero beside either footprint column");
    }
    {   // P02: a two-frame arrow still propagates on its frame 2 and reaches three tiles.
        std::vector<EnemyStats> stats{dummy_stats()};
        stats[0].attack_kind=3;stats[0].move_delay=100;stats[0].aggro_range=10;stats[0].attack=200;
        CombatWorld world;world.reset(12,12,std::vector<std::int8_t>(144,0),&stats);
        EnemyFrames frames{2,2,4};frames.effect=2;world.set_enemy_frames(0,frames);
        world.place_enemy(0,0,5,2); // facing down; the hero stands three tiles below
        auto hero=hero_at(5,5,2);hero.hp=hero.max_hp=10000;hero.defense=0;
        CombatEvents events;
        for(int i=0;i<80 && hero.hp==10000;++i) world.tick(hero,events);
        check(hero.hp<10000,"two-frame projectile reaches distance 3");
    }
    {   // E01: a reach-3 strike plays the enemy's own atef on the hero (aw9) besides aw6.
        std::vector<EnemyStats> stats{dummy_stats()};
        stats[0].attack_kind=2;stats[0].move_delay=100;stats[0].aggro_range=10;
        CombatWorld world;world.reset(12,12,std::vector<std::int8_t>(144,0),&stats);
        world.set_enemy_frames(0,{2,2,4});
        world.place_enemy(0,0,5,3); // facing down, two tiles above the hero
        auto hero=hero_at(5,5,2);hero.hp=hero.max_hp=10000;hero.defense=0;
        bool seen=false;
        for(int i=0;i<80 && !seen;++i) {
            CombatEvents events;world.tick(hero,events);
            for(const auto& fx:world.battle_effects()) seen|=fx.kind==kEifEffectBase && fx.target==-1;
        }
        check(seen,"reach attack plays aw9 with the enemy EIF on the hero");
    }
    {   // G02 + G03: instant kill pops exactly max HP; the summon follows the guardian's level.
        std::vector<EnemyStats> stats{dummy_stats()};
        stats[0].hp=1000;stats[0].element=1;stats[0].aggro_range=0;stats[0].move_delay=100;
        CombatWorld world;world.reset(20,20,std::vector<std::int8_t>(400,0),&stats);
        auto hero=hero_at(10,10,1);hero.guardian_id=3;hero.guardian_level=30;hero.guardian_calm=2;
        check(summon(world,hero),"guardian 3 skill 2 summoned");
        world.place_enemy(0,0,10,7); // two tiles ahead of the guardian tile (10,9)
        hero.guardian_level=31;
        int largest=0;
        for(int i=0;i<200 && world.enemies()[0].state!=EnemyState::dying;++i) {
            CombatEvents events;world.tick(hero,events);
            if(world.guardian().state!=0) check(world.guardian().level==31,"summon reads the guardian's current level");
            for(const auto& popup:world.popups()) largest=std::max(largest,popup.value);
        }
        check(world.enemies()[0].state==EnemyState::dying && largest==1000,"al.c -> al.b(max HP): popup equals max HP");
        check(!world.enemies()[0].hidden && world.enemies()[0].frame<=0,"al.c clears hidden; al.b death starts at frame -1");
    }
    {   // G04: during the channel (p.i == 2) an enemy in range may pick the guardian (h.a(0,9) < 7).
        std::vector<EnemyStats> stats{dummy_stats()};
        stats[0].aggro_range=4;stats[0].move_delay=1;stats[0].hp=100000;
        CombatWorld world;world.reset(20,20,std::vector<std::int8_t>(400,0),&stats);
        auto hero=hero_at(10,12,1);hero.guardian_id=0;hero.guardian_level=30;hero.guardian_calm=2; // 10-loop channel
        check(summon(world,hero),"guardian 0 skill 2 summoned");
        world.place_enemy(0,0,13,9); // in range of both; al.a(ao,p) asks about the guardian first
        bool chased=false;
        for(int i=0;i<200 && world.guardian().state!=0;++i) {
            CombatEvents events;world.tick(hero,events);
            chased|=world.enemies()[0].target==2;
        }
        check(chased,"enemy chases the channelling guardian");
    }
    {   // P03: a reused slot does not inherit the old shooter's projectile (no reflect on the newcomer).
        std::vector<EnemyStats> stats{dummy_stats()};
        stats[0].attack_kind=3;stats[0].move_delay=100;stats[0].aggro_range=10;stats[0].hp=1000;
        CombatWorld world;world.reset(20,20,std::vector<std::int8_t>(400,0),&stats);
        EnemyFrames frames{2,2,4};frames.effect=2;world.set_enemy_frames(0,frames);
        world.place_enemy(0,0,5,2);
        auto hero=hero_at(5,5,2);hero.hp=hero.max_hp=10000;hero.defense=0;
        for(int i=0;i<40 && world.projectiles().empty();++i) {CombatEvents events;world.tick(hero,events);}
        check(!world.projectiles().empty(),"shooter fired");
        world.hit_enemy(0,5000,false,1,false,1,-1,hero);
        for(int i=0;i<8;++i) {CombatEvents events;world.tick(hero,events);}
        world.place_enemy(0,0,15,15);
        check(world.enemies().size()==1 && world.enemies()[0].tile_x==15,"newcomer reuses the slot");
        for(int i=0;i<12;++i) {CombatEvents events;world.tick(hero,events);}
        check(hero.hp<10000 && world.target_enemy()==nullptr,"old projectile hits the hero but does not target the newcomer");
    }
    {   // E02: the visible rectangle drives al.f; an idle enemy off screen stops its update.
        std::vector<EnemyStats> stats{dummy_stats()};
        stats[0].aggro_range=0;stats[0].move_delay=1;
        CombatWorld world;world.reset(64,64,std::vector<std::int8_t>(4096,0),&stats);
        world.place_enemy(0,0,40,40);
        world.set_visible_area(0,0,320,240);
        auto hero=hero_at(2,2,1);
        for(int i=0;i<40;++i) {CombatEvents events;world.tick(hero,events);}
        check(!world.enemies()[0].on_screen && world.enemies()[0].tile_x==40 && world.enemies()[0].tile_y==40,
              "off-screen idle enemy keeps still");
    }
}

static void world_regressions() {
    // W04: bu.run -> ae.d places the map's enemies on their own tiles before any tick (event or not).
    std::vector<EnemyStats> stats{dummy_stats()};
    for (std::uint32_t seed = 1; seed <= 4; ++seed) {
        CombatWorld world; world.reset(20, 20, std::vector<std::int8_t>(400, 0), &stats, seed);
        world.set_enemy_frames(0, {2, 2, 4});
        for (int i = 0; i < 8; ++i) world.queue_spawn(0, 0, 2 + 2 * i, 3);
        world.queue_spawn(0, 0, 10, 10); // the hero's tile: ae.a retries with a scatter
        auto hero = hero_at(10, 10, 1);
        CombatEvents events; world.event_tick(hero, events);
        check(world.enemies().size() == 9, "initial spawns happen on the first (event) tick");
        const auto at = [&](int x, int y) {
            return std::count_if(world.enemies().begin(), world.enemies().end(),
                                 [&](const auto& e) { return e.tile_x == x && e.tile_y == y; });
        };
        for (int i = 0; i < 8; ++i) check(at(2 + 2 * i, 3) == 1, "initial spawn keeps the EVT tile");
        check(at(10, 10) == 0, "blocked initial spawn moves off the hero");
    }
}

static void enemy_regressions() {
    { // EN02 al.a(...): knockback rewrites o.i only; the sprite keeps facing o.j
        std::vector<EnemyStats> stats{dummy_stats()};
        CombatWorld world;world.reset(32,32,std::vector<std::int8_t>(1024,0),&stats,7);
        world.place_enemy(0,0,5,5);
        auto hero=hero_at(25,25,4);
        world.hit_enemy(0,0,true,4,false,1,-1,hero);
        const auto& e=world.enemies()[0];
        check(e.state==EnemyState::knockback && e.direction==2 && e.heading()==4,"knockback keeps facing, moves along o.i");
        CombatEvents events;world.tick(hero,events);
        check(world.enemies()[0].x_px==5*16+8 && world.enemies()[0].direction==2,"knockback steps right without turning");
    }
    { // EN01 al.o: the summon timer still runs while the summoner dies
        std::vector<EnemyStats> stats{dummy_stats()};
        stats[0].summoner=true;stats[0].summon_element=1;stats[0].hp=10000;
        CombatWorld world;world.reset(32,32,std::vector<std::int8_t>(1024,0),&stats,123);
        world.place_enemy(0,0,15,15);
        auto hero=hero_at(1,1,1);hero.agility=1000;
        world.hit_enemy(0,0,false,1,false,0,-1,hero);
        check(world.enemies()[0].summon_timer==40,"summon timer armed");
        for(int i=0;i<37;++i) {CombatEvents events;world.tick(hero,events);}
        world.hit_enemy(0,20000,false,1,false,0,-1,hero);
        for(int i=0;i<3;++i) {CombatEvents events;world.tick(hero,events);}
        check(world.enemies().size()==2 && world.enemies()[0].state==EnemyState::dying,"dying summoner still summons");
        const auto& a=world.enemies()[0];const auto& b=world.enemies()[1];
        check(b.tile_x!=a.tile_x || b.tile_y!=a.tile_y,"the corpse still blocks its tile");
    }
    { // EN03 al.r: the idle branch is a(true), a full rest with cooldowns
        std::vector<EnemyStats> stats{dummy_stats()};
        CombatWorld world;world.reset(64,64,std::vector<std::int8_t>(64*64,0),&stats,99);
        world.place_enemy(0,0,32,32);
        auto hero=hero_at(1,1,1);
        bool idle_ready=false;
        for(int i=0;i<300;++i) {
            CombatEvents events;world.tick(hero,events);
            const auto& e=world.enemies()[0];
            const bool inner=e.tile_x>1 && e.tile_x<62 && e.tile_y>1 && e.tile_y<62;
            const bool now=e.state==EnemyState::idle && e.move_cooldown==0;
            check(!(inner && now && idle_ready),"a wandering enemy rests before thinking again");
            idle_ready=now;
        }
    }
}

int main(int argc, char** argv) {
    try {
        synthetic();
        hero_attack();
        combo_length();
        guardian();
        summon_reallocation();
        cancelled_guardian();
        battle_presentation();
        guardian_attack_elements();
        critical_weapon_rules();
        hit_options();
        poison();
        hero_death();
        guardian_summon_while_dying();
        pickup();
        audit_regressions();
        world_regressions();
        enemy_regressions();
        if (argc > 1) {
            GameArchive archive(argv[1]);
            for (int i = 0; i < 3; ++i) {
                const auto table = parse_enemy_table(archive.read_entry("enm/data" + std::to_string(i)));
                check(table.size() == 43, "43 enemy records per table");
                check(table[42].hp > 9000 && table[42].drops.empty(), "last record is the heavy unit");
            }
            const auto base = parse_enemy_table(archive.read_entry("enm/data0"));
            bosses(base);
            check(base[0].name_text == 556 && base[0].level == 40 && base[0].hp == 1830 &&
                  base[0].attack == 130 && base[0].respawn_delay == 320, "record 0 fields");
        }
        std::cout << "combat tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "combat test failed: " << error.what() << '\n';
        return 1;
    }
}
