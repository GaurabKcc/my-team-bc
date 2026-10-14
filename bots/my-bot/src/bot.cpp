// e0051d0 survival base + exception guards + Willow self-heal fix + sectors.
// Power shards (strength <=3 team, health otherwise); banks on any carry.
#include <cambc.h>
#include "bot.h"
#include <array>
#include <iostream>
#include "random.h"
#include <cstddef>
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
    try {
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
    } catch (...) {
        return false;
    }
}

template<class Target>
bool attack_if_in_range(const Target& target) {
    auto& me = cambc::self();
    if (!me.can_attack() || !target.is_visible() || target.health() <= 0
        || !me.attack_tiles().is_set(target.position()))
        return false;
    try {
        me.attack(target);
    } catch (...) {
        return false;
    }
    return true;
}

bool use_attack_reset() {
    auto& me = cambc::self();
    if (!me.can_cast()) return false;
    for (auto& ability : me.abilities())
        if (ability.is_ready() && ability.is_attack_reset() && ability.can_target_self()) {
            try {
                return ability.use();
            } catch (...) {
                return false;
            }
        }
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
            try {
                if (ability.use())
                    return true;
            } catch (...) {
            }
        }
        if (ability.can_target_allies()) {
            for (const auto& ally : cambc::vision::champions(team::ally))
                if (ally.is_visible() && ally.health() > 0 && ally.health() < ally.max_health() * 0.6f
                    && ability.valid_tiles().is_set(ally.position())) {
                    // Never target ourselves through the ally mask: allies
                    // excludes self and the engine rejects it every turn.
                    try {
                        if (ally.position().x == me.position().x
                            && ally.position().y == me.position().y)
                            continue;
                    } catch (...) {
                        continue;
                    }
                    try {
                        if (ability.use(ally))
                            return true;
                    } catch (...) {
                    }
                }
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

bool under_threat() {
    auto& me = cambc::self();
    for (const auto& camp : cambc::vision::camps()) {
        if (camp.health() > 0 && distance(me.position(), camp.position()) <= camp.attack_range())
            return true;
    }
    for (const auto& enemy : cambc::vision::champions(team::enemy)) {
        if (!enemy.is_visible() || enemy.health() <= 0)
            continue;
        if (distance(me.position(), enemy.position()) <= enemy.attack_range() + 1)
            return true;
    }
    return false;
}

// A group is worth walking to unless we can see it respawning. Groups out of
// vision are assumed alive: walking over is how we find out.
bool group_farmable(const cambc::map::camp_group& group, const cambc::bitboard& visible) {
    if ((group.spawn_tiles() & visible).is_empty())
        return true;
    try {
        return !std::holds_alternative<cambc::map::camp_group_respawning>(group.state());
    } catch (...) {
        return true;
    }
}

} // namespace

// Read the draft's opening plan tail and find this champion's camp group.
// Tail layout after the text plan and its NUL: magic "DPLN", version u8,
// count u8, then (champion id, group, pack, flags) per pick in pick order.
// Anything malformed falls back to no assignment (nearest-first farming).
bool Bot::read_assignment() {
    if (assignment_read)
        return my_group >= 0;
    assignment_read = true;
    my_group = -1;

    const auto& mem = cambc::draft_memory;
    std::size_t off = 0;
    while (off < mem.size() && mem[off] != std::byte{0})
        ++off;
    if (off + 6 >= mem.size())
        return false;
    if (mem[off + 1] != std::byte{'D'} || mem[off + 2] != std::byte{'P'}
        || mem[off + 3] != std::byte{'L'} || mem[off + 4] != std::byte{'N'})
        return false;
    if (mem[off + 5] != std::byte{1})
        return false;
    const int count = std::to_integer<int>(mem[off + 6]);
    const std::size_t base = off + 7;
    if (count <= 0 || base + 4 * static_cast<std::size_t>(count) > mem.size())
        return false;

    // champion_type -> champion_draft. Cassi and Issac are two bodies for one
    // pick and both claim that pick's group.
    const auto name = cambc::self().champion_name();
    const int raw = static_cast<int>(name);
    int want;
    if (raw <= 2)
        want = raw;
    else if (raw == 3 || raw == 4)
        want = static_cast<int>(cambc::champion_draft::cassi_and_issac);
    else
        want = raw - 1;

    for (int i = 0; i < count; ++i) {
        if (std::to_integer<int>(mem[base + 4 * i]) == want) {
            my_group = std::to_integer<int>(mem[base + 4 * i + 1]);
            return true;
        }
    }
    return false;
}

cambc::bitboard Bot::assigned_target() {
    if (my_group < 0)
        return cambc::bitboard{};
    const auto visible = cambc::vision::visible_tiles();
    int seen = 0;
    for (const auto& group : cambc::map::camp_groups()) {
        if (seen++ != my_group)
            continue;
        if (!group_farmable(group, visible))
            break;
        return group.spawn_tiles();
    }
    return cambc::bitboard{};
}

void Bot::farm_camps(bool engage_shooter) {
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
        // Own sector first, so teammates spread out instead of converging on
        // the same camp. Falls back to every farmable group when ours is
        // dead, respawning, or unreachable.
        if (read_assignment()) {
            goals = assigned_target();
            if (goals.is_empty()) {
                const auto visible = cambc::vision::visible_tiles();
                for (const auto& group : cambc::map::camp_groups()) {
                    const bool group_visible = !(group.spawn_tiles() & visible).is_empty();
                    if (group_visible && std::holds_alternative<cambc::map::camp_group_respawning>(group.state()))
                        continue;
                    goals |= group.spawn_tiles();
                }
            }
        } else {
            const auto visible = cambc::vision::visible_tiles();
            for (const auto& group : cambc::map::camp_groups()) {
                const bool group_visible = !(group.spawn_tiles() & visible).is_empty();
                if (group_visible && std::holds_alternative<cambc::map::camp_group_respawning>(group.state()))
                    continue;
                goals |= group.spawn_tiles();
            }
        }
    }

    // Re-check camps after moving, but never cast there: "excludes walking"
    // spells throw once we have moved (engine error 126) and waste the turn.
    if (move_towards_closest(goals))
        attack_camps();
}

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

    if (!me.is_alive()) {
        // Shop while dead: rules allow purchases on spawn OR while dead, so a
        // 60-round death is otherwise 60 wasted shopping turns. Measured
        // neutral (9-9) in mirrored test; kept as a pure turn-conversion.
        const std::array items{cambc::item_type::power_shard_of_strength,
            cambc::item_type::power_shard_of_focus, cambc::item_type::power_shard_of_health};
        const auto item = items[chosen_stat];
        const unsigned count = me.gold_count() / item.cost();
        try {
            if (count && me.can_buy(item, count))
                me.buy(item, count);
        } catch (...) {
        }
        return;
    }
    // Shop every chance: attempt BEFORE the action guard (a spawn-standing
    // champ with spent move/attack may still shop) and use Tavi's passive
    // (shards purchasable anywhere on the map). Mirrored test: neutral 9-9,
    // kept as a missed-chance conversion like shop-while-dead.
    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(me.position());
    if (at_spawn || me.champion_name() == cambc::champion_type::tavi) {
        const std::array items{cambc::item_type::power_shard_of_strength,
            cambc::item_type::power_shard_of_focus, cambc::item_type::power_shard_of_health};
        const auto item = items[chosen_stat];
        const unsigned count = me.gold_count() / item.cost();
        try {
            if (count && me.can_buy(item, count)) me.buy(item, count);
        } catch (...) {
        }
    }

    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()
        || (!me.can_move() && !me.can_attack()))
        return;

    // SURVIVAL: run clear of damage first, then recall. Recall is cancelled
    // by any hit, so recalling while a camp or enemy can still reach us is how
    // the team gets pummeled. Carriers bail out sooner: dying donates the bag.
    if (!at_spawn) {
        const float bail = me.gem_count() > 0 ? 0.5f : 0.3f;
        if (me.health() < me.max_health() * bail) {
            if (under_threat())
                move_towards_closest(cambc::map::spawn_region(team::ally));
            else
                me.recall();
            return;
        }
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

    // Submit only a FULL bag (>=40 gems): light carriers keep acting, full
    // carriers become the submitters - the team splits by bag size. Measured
    // 10-8 over bank-on-any-gem (swept interlock+rows).
    if (me.gem_count() >= 40) {
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