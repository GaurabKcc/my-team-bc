// Minimal winning bot: gaurab's 9-0 logic + Monster->Home emblems + ward-first
#include <cambc.h>
#include "bot.h"
#include <array>
#include <iostream>
#include "random.h"
#include <cstdint>
#include <span>

using cambc::vec2;
using cambc::team;

namespace {

int distance(vec2 a, vec2 b) {
    return static_cast<int>(a.chebyshev_distance(b));
}

cambc::bitboard tiles_around(vec2 position, int radius) {
    cambc::bitboard tiles;
    tiles.clear();
    tiles.draw_chebyshev(position, radius);
    return tiles;
}

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
    const auto blocked = blocked_tiles();
    goals &= ~blocked | cambc::vision::camp_tiles();
    if (goals.is_empty())
        return false;
    if (goals.count() == 1 && !blocked.is_set(goals.positions()[0]))
        return me.move_towards(goals.positions()[0]);
    const auto path_walls = blocked & ~goals;
    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute_to(path_walls, goals, tiles_around(me.position(), 0), visited);
    auto step = me.position();
    for (auto tile : blocked.reachable_tiles(step, me.base_stats().base_move_speed).positions())
        if (field.get(tile) < field.get(step)) step = tile;
    if (step != me.position())
        return me.move_absolute(step);
    vec2 nearest = goals.positions()[0];
    for (auto tile : goals.positions())
        if (field.get(tile) < field.get(nearest)) nearest = tile;
    return me.move_towards(nearest);
}

template<class Target>
bool attack_if_in_range(const Target& target) {
    auto& me = cambc::self();
    if (!me.can_attack() || !target.is_visible() || target.health() <= 0
        || !me.attack_tiles().is_set(target.position()))
        return false;
    me.attack(target);
    return true;
}

bool use_attack_reset() {
    auto& me = cambc::self();
    if (!me.can_cast()) return false;
    for (auto& ability : me.abilities())
        if (ability.is_ready() && ability.is_attack_reset() && ability.can_target_self())
            return ability.use();
    return false;
}

bool cast_offensive_ability() {
    auto& me = cambc::self();
    if (!me.can_cast()) return false;
    auto abilities = me.abilities();
    for (auto& ability : abilities) {
        if (!ability.is_ready()) continue;
        if (!ability.is_damage() && !ability.is_aoe_damage() && !ability.is_cc()) continue;
        if (ability.can_target_camps() || ability.can_target_enemies()) {
            if (ability.can_target_camps())
                for (const auto& target : cambc::vision::camps())
                    if (target.health() > 0 && ability.valid_tiles().is_set(target.position())
                        && ability.use(target))
                        return true;
            if (ability.can_target_enemies())
                for (const auto& target : cambc::vision::champions(team::enemy))
                    if (target.is_visible() && target.health() > 0
                        && ability.valid_tiles().is_set(target.position()) && ability.use(target))
                        return true;
        }
        if (ability.can_target_ground()) {
            for (const auto& target : cambc::vision::camps())
                if (target.health() > 0 && ability.valid_tiles().is_set(target.position())
                    && ability.use(target.position()))
                    return true;
            for (const auto& target : cambc::vision::champions(team::enemy))
                if (target.is_visible() && target.health() > 0
                    && ability.valid_tiles().is_set(target.position()) && ability.use(target.position()))
                    return true;
        }
    }
    return false;
}

bool cast_defensive_ability() {
    auto& me = cambc::self();
    if (!me.can_cast()) return false;
    for (auto& ability : me.abilities()) {
        if (!ability.is_ready()) continue;
        if (!ability.is_heal() && !ability.is_shield()) continue;
        if (ability.can_target_self()) {
            if (ability.use())
                return true;
        }
        if (ability.can_target_allies()) {
            for (const auto& ally : cambc::vision::champions(team::ally))
                if (ally.is_visible() && ally.health() > 0 && ally.health() < ally.max_health() * 0.6f
                    && ability.valid_tiles().is_set(ally.position()) && ability.use(ally))
                    return true;
        }
    }
    return false;
}

bool attack_camps() {
    for (const auto& camp : cambc::vision::camps())
        if (attack_if_in_range(camp)) {
            if (use_attack_reset())
                for (const auto& again : cambc::vision::camps())
                    if (attack_if_in_range(again)) break;
            return true;
        }
    return false;
}

void farm_camps(bool engage_shooter) {
    cast_offensive_ability();

    if (attack_camps())
        return;

    cambc::bitboard goals;
    goals.clear();

    if (engage_shooter) {
        const int range = cambc::self().attack_range();
        for (const auto& camp : cambc::vision::camps())
            if (camp.health() > 0)
                goals |= tiles_around(camp.position(), range);
    }

    if (goals.is_empty()) {
        const auto visible = cambc::vision::visible_tiles();
        for (const auto& group : cambc::map::camp_groups()) {
            const bool group_visible = !(group.spawn_tiles() & visible).is_empty();
            if (group_visible && std::holds_alternative<cambc::map::camp_group_respawning>(group.state()))
                continue;
            goals |= group.spawn_tiles();
        }
    }

    if (move_towards_closest(goals)) {
        cast_offensive_ability();
        attack_camps();
    }
}

} // namespace

void Bot::play_turn() {
    // Initialise build: power shards of strength scale our basic attacks.
    // Small drafts (≤3) fight short, burst-heavy skirmishes -> stack strength.
    // Larger drafts are attrition fights -> health shards keep champs alive.
    if (chosen_stat < 0) {
        chosen_stat = cambc::draft::team_size() <= 3 ? 0 : 2;
    }

    std::uint32_t round = 0;
    try {
        cambc::shared_memory.read(0, std::span{&round, 1});
    } catch (...) {
    }
    round = cambc::round_number();
    try {
        cambc::shared_memory.write(0, std::span{&round, 1});
    } catch (...) {
    }

    auto& me = cambc::self();

    if (!me.is_alive() || me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()
        || (!me.can_move() && !me.can_attack()))
        return;

    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(me.position());
    if (at_spawn) {
        auto& me2 = cambc::self();
        const std::array items{cambc::item_type::power_shard_of_strength,
            cambc::item_type::power_shard_of_focus, cambc::item_type::power_shard_of_health};
        const auto item = items[chosen_stat];
        const unsigned count = me2.gold_count() / item.cost();
        if (count && me2.can_buy(item, count)) me2.buy(item, count);
    }

    // Recall immediately when badly hurt away from spawn.
    if (!at_spawn && me.health() < me.max_health() * 0.35f) {
        me.recall();
        return;
    }

    // Pop a heal or shield before brawling or farming while not critically hurt.
    if (me.health() < me.max_health() * 0.6f)
        cast_defensive_ability();

    // Brawl: swing at an adjacent enemy and cash an attack-reset for a second swing.
    for (const auto& enemy : cambc::vision::champions(team::enemy))
        if (attack_if_in_range(enemy)) {
            if (use_attack_reset())
                for (const auto& again : cambc::vision::champions(team::enemy))
                    if (attack_if_in_range(again)) break;
            return;
        }

    // Bank any carried gems before farming.
    if (me.gem_count() > 0) {
        cambc::bitboard goals;
        goals.clear();
        for (auto altar : cambc::map::altars().positions()) {
            if (distance(me.position(), altar) <= cambc::constants::submit_gems_range
                && !cambc::vision::has_altar_lock(altar)) {
                me.submit_gems(altar);
                return;
            }
            if (!cambc::vision::has_altar_lock(altar)) goals |= tiles_around(altar, cambc::constants::submit_gems_range);
        }
        move_towards_closest(goals);
        return;
    }

    // Only chase a distant shooter when already hurt enough to matter.
    farm_camps(false);

    me.set_priority(static_cast<std::int32_t>(1 + random_number(100)));
}