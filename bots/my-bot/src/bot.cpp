// Rush-tribute champion: farm -> solo-hold capture -> courier small batches.
// Beats the random starter on tribute throughput, not kills.
#include <cambc.h>
#include "bot.h"
#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <variant>

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
        return step != me.position() && me.move_absolute(step);
    } catch (...) {
        // Known-occupied or stale-vision destination: keep the turn, try
        // something else next time instead of yielding.
        return false;
    }
}

template <class Target>
bool attack_if_in_range(const Target& target) {
    auto& me = cambc::self();
    if (!me.can_attack() || !target.is_visible() || target.health() <= 0
        || !me.attack_tiles().is_set(target.position()))
        return false;
    try {
        me.attack(target);
    } catch (...) {
        // Stale snapshot (target died or left vision between query and
        // attack): treat as out of range, keep the turn.
        return false;
    }
    return true;
}

bool is_melee_champion(cambc::champion_type t) {
    using ct = cambc::champion_type;
    return t == ct::ryth || t == ct::vendetta || t == ct::issac || t == ct::marcella
        || t == ct::ren || t == ct::varg;
}

// Heavy groups (Elk 60dmg, Monolith 200dmg) shred pre-emblem champions.
// Only engage them once the first emblem is done.
bool group_too_heavy(const cambc::map::camp_group& group, int primary_buys) {
    if (primary_buys >= 5)
        return false;
    for (const auto& camp : group.camps())
        if (camp.is_elk() || camp.is_monolith())
            return true;
    return false;
}
long group_score(const cambc::map::camp_group& group, vec2 from) {
    // Travel dominates melee throughput: one gem is worth about two tiles of
    // walking, so champions work tight local loops instead of cross-map
    // treks. (Nearest-first with a value tiebreak.)
    long dist = 0;
    bool first = true;
    for (auto tile : group.spawn_tiles().positions()) {
        int d = distance(from, tile);
        if (first || d < dist) {
            dist = d;
            first = false;
        }
    }
    long score = static_cast<long>(group.gem_value()) * 2L
        + static_cast<long>(group.gold_value()) / 30L - dist;
    if (group.gem_value() >= 150)
        score /= 20;
    return score;
}

} // namespace

void Bot::init_once() {
    initialised = true;
    auto& me = cambc::self();
    const auto name = me.champion_name();
    is_melee = is_melee_champion(name);
    is_gold_farmer = name == cambc::champion_type::vesper_vale
        || name == cambc::champion_type::tavi || name == cambc::champion_type::prism;
    if (cambc::draft_memory.size() >= 3) {
        const auto* d = cambc::draft_memory.data();
        std::cout << "draft plan: " << static_cast<char>(d[0]) << static_cast<char>(d[1])
                  << static_cast<char>(d[2]) << '\n';
    }
}

void Bot::buy_items() {
    auto& me = cambc::self();
    // Melee clearers: Monster strength (first emblem = +10% camp damage).
    // Gold farmers: Treasure strength (funds the team, Vesper stacks to 40%).
    // After the first emblem (5 shards), Home strength (faster recall).
    cambc::item_type primary = is_gold_farmer ? cambc::item_type::treasure_shard_of_strength
                                              : cambc::item_type::monster_shard_of_strength;
    if (primary_buys >= 5)
        primary = cambc::item_type::home_shard_of_strength;
    try {
        const unsigned count = me.gold_count() / primary.cost();
        if (count && me.can_buy(primary, count)) {
            me.buy(primary, count);
            primary_buys += static_cast<int>(count);
        }
    } catch (...) {
    }
}

void Bot::maybe_ward() {
    auto& me = cambc::self();
    try {
        if (me.ward_cooldown() != 0 || !me.can_move())
            return;
        auto placement = me.ward_placement_tiles();
        if (placement.is_empty())
            return;
        // Pin vision on the best-scoring group's tiles if reachable.
        long best = -1;
        vec2 dest = placement.positions()[0];
        bool found = false;
        for (const auto& group : cambc::map::camp_groups()) {
            long s = group_score(group, me.position());
            if (s > best) {
                auto tiles = group.spawn_tiles() & placement;
                if (!tiles.is_empty()) {
                    best = s;
                    dest = tiles.positions()[0];
                    found = true;
                }
            }
        }
        if (!found)
            return;
        me.place_ward(dest);
    } catch (...) {
    }
}

// Spend ready abilities: resets enable a bonus basic, damage burns camps,
// shields/heals keep the push alive, CC breaks enemy channels.
void use_abilities() {
    auto& me = cambc::self();
    if (!me.can_cast())
        return;
    for (const auto& ab : me.abilities()) {
        try {
            if (!ab.is_ready())
                continue;
            if (ab.is_attack_reset()) {
                if (ab.use())
                    for (const auto& camp : cambc::vision::camps())
                        if (attack_if_in_range(camp))
                            break;
                return; // one successful cast per turn
            }
            if (ab.is_shield()) {
                if (me.health() < me.max_health() * 0.6f && ab.use())
                    return;
                continue;
            }
            if (ab.is_heal()) {
                for (const auto& ally : cambc::vision::champions(team::ally)) {
                    try {
                        if (ally.health() < ally.max_health() * 0.6f && ab.use(ally))
                            return;
                    } catch (...) {
                    }
                }
                continue;
            }
            if (ab.is_cc()) {
                for (const auto& enemy : cambc::vision::champions(team::enemy)) {
                    try {
                        if (ab.valid_tiles().is_set(enemy.position()) && ab.use(enemy))
                            return;
                    } catch (...) {
                    }
                }
                continue;
            }
            if (ab.is_dash())
                continue; // movement handles positioning
            if (ab.is_damage() || ab.is_aoe_damage()) {
                bool cast = false;
                for (const auto& camp : cambc::vision::camps()) {
                    try {
                        if (ab.valid_tiles().is_set(camp.position()) && ab.use(camp)) {
                            cast = true;
                            break;
                        }
                    } catch (...) {
                    }
                }
                if (cast)
                    return;
            }
        } catch (...) {
        }
    }
}

// Hold a reward-ready region: never step off (empty = timer reset).
// Also step onto a nearby claimable region instead of leaving it empty.
bool hold_capture() {
    auto& me = cambc::self();
    const auto visible = cambc::vision::visible_tiles();
    for (const auto& group : cambc::map::camp_groups()) {
        bool claimable = false;
        try {
            // state() throws when the group is not in vision; like the
            // starter, only query groups with a visible spawn tile.
            if ((group.spawn_tiles() & visible).is_empty())
                continue;
            claimable =
                std::holds_alternative<cambc::map::camp_group_reward_available>(group.state());
        } catch (...) {
            continue;
        }
        if (!claimable)
            continue;
        if (!group.spawn_tiles().is_set(me.position())) {
            // Walk onto the region and hold it; do not bank mid-claim.
            move_towards_closest(group.spawn_tiles());
            me.set_priority(85);
            return true;
        }
        for (const auto& enemy : cambc::vision::champions(team::enemy))
            if (attack_if_in_range(enemy))
                return true;
        use_abilities();
        me.set_priority(90);
        return true;
    }
    return false;
}

void Bot::play_turn() {
    if (!initialised)
        init_once();

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
    if (at_spawn)
        buy_items();
    maybe_ward();

    // Hurt? Break contact first: recall cancels on any damage, so recalling
    // adjacent to camps just feeds kills. Retreat out of camp range, then
    // recall once safe. Carriers retreat earlier to protect the bag.
    const float retreat_at = me.gem_count() > 0 ? 0.55f : 0.4f;
    if (!at_spawn && me.health() < me.max_health() * retreat_at) {
        bool in_danger = false;
        for (const auto& camp : cambc::vision::camps()) {
            if (camp.health() > 0 && distance(me.position(), camp.position()) <= 4) {
                in_danger = true;
                break;
            }
        }
        if (in_danger) {
            move_towards_closest(cambc::map::spawn_region(team::ally));
            return;
        }
        try {
            me.recall();
        } catch (...) {
        }
        return;
    }

    // Never abandon a claim in progress.
    if (hold_capture())
        return;

    // Courier: bank whenever carrying. Small bags die less often mid-treks,
    // and every death donates the whole bag plus 50g to the killer.
    if (me.gem_count() > 0) {
        for (auto altar : cambc::map::altars().positions()) {
            try {
                if (distance(me.position(), altar) <= cambc::constants::submit_gems_range
                    && !cambc::vision::has_altar_lock(altar)) {
                    me.submit_gems(altar);
                    me.set_priority(90);
                    return;
                }
            } catch (...) {
            }
        }
        cambc::bitboard goals;
        goals.clear();
        for (auto altar : cambc::map::altars().positions())
            if (!cambc::vision::has_altar_lock(altar))
                goals |= tiles_around(altar, cambc::constants::submit_gems_range);
        me.set_priority(90);
        move_towards_closest(goals);
        return;
    }

    // Opportunistic defence: hit enemies already in range, then abilities.
    for (const auto& enemy : cambc::vision::champions(team::enemy))
        if (attack_if_in_range(enemy)) {
            me.set_priority(40);
            return;
        }
    use_abilities();

    // Burn the weakest camp in range first: faster kills cut incoming DPS.
    {
        bool acted = false;
        float lowest = 0;
        bool have = false;
        for (const auto& camp : cambc::vision::camps()) {
            if (!me.can_attack() || !camp.is_visible() || camp.health() <= 0
                || !me.attack_tiles().is_set(camp.position()))
                continue;
            if (!have || camp.health() < lowest) {
                lowest = camp.health();
                have = true;
            }
        }
        if (have) {
            for (const auto& camp : cambc::vision::camps()) {
                if (camp.is_visible() && camp.health() == lowest
                    && me.attack_tiles().is_set(camp.position())
                    && attack_if_in_range(camp)) {
                    acted = true;
                    break;
                }
            }
        }
        if (acted) {
            me.set_priority(is_melee ? 20 : 15);
            return;
        }
    }

    // Farm my own best-scoring group. The score is distance-weighted from my
    // position, so teammates naturally spread across groups in parallel.
    cambc::bitboard goals;
    goals.clear();
    const auto visible = cambc::vision::visible_tiles();
    long best = -1;
    cambc::bitboard best_tiles;
    best_tiles.clear();
    for (const auto& group : cambc::map::camp_groups()) {
        bool skip = false;
        try {
            const bool group_visible = !(group.spawn_tiles() & visible).is_empty();
            skip = group_visible
                && std::holds_alternative<cambc::map::camp_group_respawning>(group.state());
        } catch (...) {
            continue;
        }
        if (skip)
            continue;
        if (group_too_heavy(group, primary_buys))
            continue;
        long s = group_score(group, me.position());
        if (s > best) {
            best = s;
            best_tiles = group.spawn_tiles();
        }
    }
    goals |= best_tiles;
    if (move_towards_closest(goals))
        for (const auto& camp : cambc::vision::camps())
            if (attack_if_in_range(camp))
                break;
    me.set_priority(is_melee ? 20 : 15);
}
