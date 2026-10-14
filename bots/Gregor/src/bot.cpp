// Champion decisions. main.cpp dispatches here after the draft.
#include <cambc.h>
#include "bot.h"
#include <array>
#include <iostream>
#include "random.h"
#include <cstdint>
#include <span>
#include <variant>

using cambc::vec2;
using cambc::team;

int distance(vec2 a, vec2 b) {
    return static_cast<int>(a.chebyshev_distance(b));
}

bool is_self(const cambc::champion& other) {
    return static_cast<cambc::handle_champion_t>(other)
        == static_cast<cambc::handle_champion_t>(cambc::self());
}

// Run an engine action; a rejected action (e.g. an illegal cast or a race with a
// teammate) must not abort the rest of the turn, so report failure instead.
template <class F>
bool safely(F&& action, const char* action_name = nullptr) {
    try {
        return action();
    } catch (const std::exception& e) {
        try {
            std::cout << "r=" << cambc::round_number() << " cid=" << static_cast<std::uint32_t>(static_cast<cambc::handle_champion_t>(cambc::self()))
                      << " action=" << (action_name ? action_name : "unknown")
                      << " err=" << e.what() << '\n';
        } catch (...) {
        }
        return false;
    } catch (...) {
        try {
            std::cout << "r=" << cambc::round_number() << " cid=" << static_cast<std::uint32_t>(static_cast<cambc::handle_champion_t>(cambc::self()))
                      << " action=" << (action_name ? action_name : "unknown")
                      << " err=unknown\n";
        } catch (...) {
        }
        return false;
    }
}

// Claim partition (from the Khaos postmortem): a tile belongs to whichever
// living ally is closest, so teammates spread out instead of clumping.
bool claims(vec2 goal) {
    auto& me = cambc::self();
    const int mine = distance(me.position(), goal);
    for (const auto& ally : cambc::vision::champions(team::ally)) {
        if (is_self(ally) || !ally.is_visible() || ally.health() <= 0)
            continue;
        const int theirs = distance(ally.position(), goal);
        if (theirs < mine)
            return false;
        if (theirs == mine
            && static_cast<int>(ally.champion_name()) < static_cast<int>(me.champion_name()))
            return false;
    }
    return true;
}

cambc::bitboard tiles_around(vec2 position, int radius) {
    cambc::bitboard tiles;
    tiles.clear();
    tiles.draw_chebyshev(position, radius);
    return tiles;
}

// Combine terrain and visible occupants using native bitboard operations.
cambc::bitboard blocked_tiles() {
    auto blocked = cambc::map::walls() | cambc::map::holes() | cambc::map::altars()
        | cambc::map::spawn_region(team::enemy) | cambc::vision::champion_tiles()
        | cambc::vision::camp_tiles() | cambc::vision::teleport_tiles();
    blocked.set(cambc::self().position(), false);
    return blocked;
}

bool move_towards_closest(cambc::bitboard goals) {
    auto& me = cambc::self();
    if (!me.can_move() || goals.is_set(me.position()))
        return false;

    // Only free goals or occupied camp tiles can seed the field.
    const auto blocked = blocked_tiles();
    goals &= ~blocked | cambc::vision::camp_tiles();
    if (goals.is_empty())
        return false;
    if (goals.count() == 1 && !blocked.is_set(goals.positions()[0]))
        return safely([&] { return me.move_towards(goals.positions()[0]); });

    // Expand from the goals until our current tile is reached.
    const auto path_walls = blocked & ~goals;
    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute_to(path_walls, goals, tiles_around(me.position(), 0), visited);

    // Choose a reachable step with a smaller distance to the goal.
    auto step = me.position();
    for (auto tile : blocked.reachable_tiles(step, me.base_stats().base_move_speed).positions())
        if (field.get(tile) < field.get(step)) step = tile;
    if (step != me.position())
        return safely([&] { return me.move_absolute(step); });

    // Local minimum (e.g. a goal boxed in by terrain): nudge greedily toward the
    // nearest goal rather than idling in place for the rest of the game.
    vec2 nearest = goals.positions()[0];
    for (auto tile : goals.positions())
        if (field.get(tile) < field.get(nearest)) nearest = tile;
    return safely([&] { return me.move_towards(nearest); });
}

void Bot::buy_items() {
    auto& me = cambc::self();
    const std::array items{cambc::item_type::power_shard_of_strength,
        cambc::item_type::power_shard_of_focus, cambc::item_type::power_shard_of_health};
    const auto item = items[chosen_stat];
    const unsigned count = me.gold_count() / item.cost();
    if (count && me.can_buy(item, count)) me.buy(item, count);
}

// Make one basic attack if the target is already in attack range.
template<class Target>
bool attack_if_in_range(const Target& target, bool* attack_spent_flag = nullptr) {
    auto& me = cambc::self();
    if (!me.can_attack() || !target.is_visible() || target.health() <= 0
        || !me.attack_tiles().is_set(target.position())) {
        return false;
    }
    if (safely([&] { me.attack(target); return true; })) {
        if (attack_spent_flag) *attack_spent_flag = true;
        return true;
    }
    return false;
}

// Cast the first ready attack-reset so we can swing our basic attack again.
bool use_attack_reset(bool attack_spent) {
    auto& me = cambc::self();
    if (!attack_spent || !me.can_cast()) return false;
    for (auto& ability : me.abilities())
        if (ability.is_ready() && ability.is_attack_reset() && ability.can_target_self())
            return safely([&] { return ability.use(); });
    return false;
}

// Cast the first ready damage/area/cc ability onto an in-range camp or enemy.
bool cast_offensive_ability(bool did_move) {
    auto& me = cambc::self();
    if (!me.can_cast()) return false;
    auto abilities = me.abilities();
    for (auto& ability : abilities) {
        if (!safely([&] { return ability.is_ready(); })) continue;
        // If we've moved this turn, cannot cast abilities that are exclusive with movement
        if (did_move) {
            const bool excl = ability.info() && std::visit([](auto&& v){ return v.exclusive_with_movement; }, *ability.info());
            if (excl) continue;
        }
        if (!ability.is_damage() && !ability.is_aoe_damage() && !ability.is_cc()) continue;
        const auto in_range = [&](vec2 tile) { return ability.valid_tiles().is_set(tile); };
        if (ability.can_target_camps())
            for (const auto& target : cambc::vision::camps()) {
                bool usable = false;
                safely([&]{ usable = target.is_visible() && target.health() > 0; return usable; }, "camp_target");
                if (usable && in_range(target.position())
                    && safely([&] { return ability.use(target); }, "ability_use"))
                    return true;
            }
        if (ability.can_target_enemies())
            for (const auto& target : cambc::vision::champions(team::enemy))
                if (target.is_visible() && target.health() > 0
                    && in_range(target.position())
                    && safely([&] { return ability.use(target); }))
                    return true;
        // Ground-targeted area effects land on one tile and splash its neighbours.
        if (ability.can_target_ground()) {
            for (const auto& target : cambc::vision::camps()) {
                bool usable = false;
                safely([&]{ usable = target.is_visible() && target.health() > 0; return usable; }, "camp_target_ground");
                if (usable && in_range(target.position())
                    && safely([&] { return ability.use(target.position()); }, "ability_use_ground"))
                    return true;
            }
            for (const auto& target : cambc::vision::champions(team::enemy))
                if (target.is_visible() && target.health() > 0
                    && in_range(target.position())
                    && safely([&] { return ability.use(target.position()); }))
                    return true;
        }
    }
    return false;
}

// Cast a ready heal or shield on ourselves or a hurt nearby ally.
bool cast_defensive_ability(bool did_move = false) {
    auto& me = cambc::self();
    if (!me.can_cast()) return false;
    for (auto& ability : me.abilities()) {
        if (!safely([&] { return ability.is_ready(); })) continue;
        if (did_move) {
            const bool excl = ability.info() && std::visit([](auto&& v){ return v.exclusive_with_movement; }, *ability.info());
            if (excl) continue;
        }
        if (!ability.is_heal() && !ability.is_shield()) continue;
        if (ability.can_target_self() && safely([&] { return ability.use(); }))
            return true;
        if (ability.can_target_allies()) {
            for (const auto& ally : cambc::vision::champions(team::ally))
                if (ally.is_visible() && ally.health() > 0 && ally.health() < ally.max_health() * 0.6f
                    && ability.valid_tiles().is_set(ally.position())
                    && safely([&] { return ability.use(ally); }))
                    return true;
        }
    }
    return false;
}

// Swing at a camp and cash in an attack-reset for a second swing the same turn.
bool attack_camps(bool attack_spent, bool& attack_spent_out) {
    for (const auto& camp : cambc::vision::camps()) {
        bool usable = false;
        safely([&]{ usable = camp.is_visible() && camp.health() > 0; return usable; }, "camp_usable");
        if (usable && attack_if_in_range(camp, &attack_spent_out)) {
            if (use_attack_reset(attack_spent_out))
                for (const auto& camp2 : cambc::vision::camps()) {
                    bool usable2 = false;
                    safely([&]{ usable2 = camp2.is_visible() && camp2.health() > 0; return usable2; }, "camp2_usable");
                    if (usable2 && attack_if_in_range(camp2, &attack_spent_out)) break;
                }
            return true;
        }
    }
    return false;
}

// Attack a camp already in range, otherwise move onto a claimed camp region.
// When being shot from out of range, close in on the visible camp instead.
void farm_camps(bool engage_shooter, bool did_move_in, bool& attack_spent_out) {
    // Try casting before moving; if we have not moved, we can cast exclusive abilities
    cast_offensive_ability(did_move_in);

    if (attack_camps(did_move_in || attack_spent_out, attack_spent_out))
        return;

    cambc::bitboard goals;
    goals.clear();

    if (engage_shooter) {
        const int range = cambc::self().attack_range();
        for (const auto& camp : cambc::vision::camps()) {
            bool usable = false;
            safely([&]{ usable = camp.is_visible() && camp.health() > 0; return usable; }, "camp_goal");
            if (usable) goals |= tiles_around(camp.position(), range);
        }
    }

    if (goals.is_empty()) {
        // Union every group's spawn tiles, skipping groups that are
        // visibly respawning.
        const auto visible = cambc::vision::visible_tiles();
        for (const auto& group : cambc::map::camp_groups()) {
            const bool group_visible = !(group.spawn_tiles() & visible).is_empty();
            if (group_visible && std::holds_alternative<cambc::map::camp_group_respawning>(group.state()))
                continue;
            goals |= group.spawn_tiles();
        }
    }

    // Fresh vision after movement may reveal a camp already in range. Some
    // spells cannot be cast after normal movement, so only swing here.
    const auto moved_now = move_towards_closest(goals);
    if (moved_now) {
        attack_camps(true, attack_spent_out);
    }
}

void Bot::play_turn() {
    // Initialise this champion's build and read the draft message once.
    if (chosen_stat < 0) {
        // Deterministic build: power shards of strength scale our basic attacks.
        // Never buy focus shards because this bot never casts abilities.
        // Small drafts fight short, burst-heavy skirmishes, so stack strength to
        // win trades. Larger drafts are attrition fights where health shards keep
        // champions alive long enough to bank their gems.
        chosen_stat = cambc::draft::team_size() <= 3 ? 0 : 2;
        const auto dm = cambc::draft_memory;
        std::string draftstr(reinterpret_cast<const char*>(dm.data()), dm.size());
        if (draftstr.length() > 120) draftstr = draftstr.substr(0, 120);
        std::cout << "draft: " << draftstr << '\n';
    }

    // Read before writing: little-endian u32 at shared-memory byte offset zero.
    std::uint32_t round = 0;
    cambc::shared_memory.read(0, std::span{&round, 1});
    std::cout << "shared round: " << round << '\n';
    round = cambc::round_number();
    cambc::shared_memory.write(0, std::span{&round, 1});

    auto& me = cambc::self();

    // Leave channels running and skip turns where we cannot act.
    if (!me.is_alive()) {
        // If we just died this turn (state change) - log once when going to dead? Hard to detect without tracking last state.
        return;
    }
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting() || (!me.can_move() && !me.can_attack()))
        return;

    // Compact logging: log only when state changes (health band or gems change)
    {
        const float h = me.health();
        const int g = me.gem_count();
        if (std::abs(h - last_log_health) > 0.1f || g != last_log_gems) {
            last_log_health = h;
            last_log_gems = g;
            // simple state marker - just record basic state
            std::cout << "r=" << cambc::round_number() << " cid=" << static_cast<std::uint32_t>(static_cast<cambc::handle_champion_t>(me))
                      << " hp=" << static_cast<int>(h) << "/" << static_cast<int>(me.max_health())
                      << " gems=" << g << '\n';
        }
    }

    // Remember progress: losing health since last turn means something is hitting us.
    const bool took_damage = has_memory && me.health() < last_health;
    last_health = me.health();
    last_position = me.position();
    has_memory = true;

    // Shop before leaving spawn.
    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(me.position());
    if (at_spawn)
        buy_items();

    // Recall immediately when badly hurt away from spawn.
    if (!at_spawn && me.health() < me.max_health() * 0.35f) {
        me.recall();
        return;
    }

    // Pop a heal or shield before brawling or farming while not critically hurt.
    if (me.health() < me.max_health() * 0.6f)
        cast_defensive_ability();

    // Brawl: swing at an adjacent enemy and cash an attack-reset for a second swing.
    did_attack = false;
    attack_spent = false;
    did_move = false;
    for (const auto& enemy : cambc::vision::champions(team::enemy)) {
        bool usable = false;
        safely([&]{ usable = enemy.is_visible() && enemy.health() > 0; return usable; }, "enemy_brawl");
        if (usable && attack_if_in_range(enemy, &attack_spent)) {
            did_attack = true;
            if (use_attack_reset(attack_spent))
                for (const auto& again : cambc::vision::champions(team::enemy))
                    { bool usable2=false; safely([&]{usable2=again.is_visible()&&again.health()>0; return usable2;},"enemy_brawl2"); if(usable2&&attack_if_in_range(again,&attack_spent)) break; }
            return;
        }
    }

    // Bank any carried gems before farming.
    if (me.gem_count() > 0) {
        // Union all altar approach tiles: the field chooses the nearest reachable altar.
        cambc::bitboard goals;
        goals.clear();
        for (auto altar : cambc::map::altars().positions()) {
            if (distance(me.position(), altar) <= cambc::constants::submit_gems_range
                && !cambc::vision::has_altar_lock(altar)) {
                    me.submit_gems(altar);
                    std::cout << "r=" << cambc::round_number() << " cid=" << static_cast<std::uint32_t>(static_cast<cambc::handle_champion_t>(me)) << " tribute start, gems=" << me.gem_count() << '\n';
                    return;
                }
            if (!cambc::vision::has_altar_lock(altar)) goals |= tiles_around(altar, cambc::constants::submit_gems_range);
        }
        const bool moved = move_towards_closest(goals);
        did_move = moved;
        return;
    }

    // Only chase a distant shooter when already hurt enough to matter.
    farm_camps(took_damage, did_move, attack_spent);

    // Per-champion priority spreads within-team turn order; seed comes from our
    // spawn tile so it is stable for a given champion.
    me.set_priority(static_cast<std::int32_t>(1 + random_number(100)));
}
