// Minimal winning bot: gaurab's 9-0 logic + Monster->Home emblems + ward-first
#include <cambc.h>
#include "bot.h"
#include <array>
#include <cstddef>
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
            if (ability.can_target_enemies())
                for (const auto& target : cambc::vision::champions(team::enemy))
                    if (target.is_visible() && target.health() > 0
                        && ability.valid_tiles().is_set(target.position()) && ability.use(target))
                        return true;
            if (ability.can_target_camps())
                for (const auto& target : cambc::vision::camps())
                    if (target.health() > 0 && ability.valid_tiles().is_set(target.position())
                        && ability.use(target))
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
            for (const auto& ally : cambc::vision::champions(team::ally)) {
                // Never target ourselves through the ally mask: allies excludes
                // self, so casting the ally-only heal at our own tile throws
                // error 30 every turn (hundreds per game) and the heal that
                // would lift us above the threshold never lands.
                try {
                    if (ally.position().x == me.position().x
                        && ally.position().y == me.position().y)
                        continue;
                } catch (...) {
                    continue;
                }
                if (ally.is_visible() && ally.health() > 0 && ally.health() < ally.max_health() * 0.6f
                    && ability.valid_tiles().is_set(ally.position())) {
                    try {
                        if (ability.use(ally))
                            return true;
                    } catch (...) {
                    }
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

// Read the draft's opening plan and find the camp group this champion owns.
// Each champion owns a different group, so the team works several camps at
// once instead of converging on one. Needs no match-time communication.
bool Bot::read_assignment() {
    if (assignment_read)
        return my_group >= 0;
    assignment_read = true;
    my_group = -1;
    my_pack = -1;

    const auto& mem = cambc::draft_memory;
    std::size_t off = 0;
    while (off < mem.size() && mem[off] != std::byte{0})
        ++off;
    if (off + 3 >= mem.size())
        return false;
    const int version = std::to_integer<int>(mem[off + 1]);
    // v1 tails carry (champion id, group) pairs and no pack: pack falls back
    // to slot parity. v2 tails carry (champion id, group, pack) triples.
    const std::size_t stride = (version == 2) ? 3 : 2;
    if (version != 1 && version != 2)
        return false;
    const int team_size = std::to_integer<int>(mem[off + 2]);
    const std::size_t base = off + 3;
    if (team_size <= 0 || base + stride * static_cast<std::size_t>(team_size) > mem.size())
        return false;

    // champion_type -> champion_draft. Cassi and Issac are two bodies for one
    // pick and both claim that pick's group and pack.
    const auto name = cambc::self().champion_name();
    const int raw = static_cast<int>(name);
    int want;
    if (raw <= 2)
        want = raw;
    else if (raw == 3 || raw == 4)
        want = static_cast<int>(cambc::champion_draft::cassi_and_issac);
    else
        want = raw - 1;

    for (int i = 0; i < team_size; ++i) {
        if (std::to_integer<int>(mem[base + stride * i]) == want) {
            my_group = std::to_integer<int>(mem[base + stride * i + 1]);
            my_pack = (stride == 3) ? std::to_integer<int>(mem[base + stride * i + 2])
                                    : (i % 2);
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

    if (!me.is_alive() || me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()
        || (!me.can_move() && !me.can_attack()))
        return;

    // Opening-speed timing: gold only arrives via captures, so first gold is
    // first capture. Emblem check runs every 100 rounds: the full
    // item_count() copy every 25 rounds was cheap per-turn but fired for all
    // champions on the same round, and sampling finer than 100 buys nothing
    // for opening-speed analysis.
    if (first_gold_round == 0 && me.gold_count() > 0)
        first_gold_round = cambc::round_number();
    if (first_emblem_round == 0 && cambc::round_number() % 100 == 0) {
        const auto inv = me.item_count();
        static const cambc::item_type kEmblems[] = {
            cambc::item_type::monster_emblem_of_strength,
            cambc::item_type::monster_emblem_of_focus,
            cambc::item_type::monster_emblem_of_health,
            cambc::item_type::power_emblem_of_strength,
            cambc::item_type::power_emblem_of_focus,
            cambc::item_type::power_emblem_of_health,
            cambc::item_type::durability_emblem_of_strength,
            cambc::item_type::durability_emblem_of_focus,
            cambc::item_type::durability_emblem_of_health,
            cambc::item_type::treasure_emblem_of_strength,
            cambc::item_type::treasure_emblem_of_focus,
            cambc::item_type::treasure_emblem_of_health,
            cambc::item_type::jungle_emblem_of_strength,
            cambc::item_type::jungle_emblem_of_focus,
            cambc::item_type::jungle_emblem_of_health,
            cambc::item_type::home_emblem_of_strength,
            cambc::item_type::home_emblem_of_focus,
            cambc::item_type::home_emblem_of_health,
            cambc::item_type::bruiser_emblem_of_strength,
            cambc::item_type::bruiser_emblem_of_focus,
            cambc::item_type::bruiser_emblem_of_health,
        };
        for (auto e : kEmblems) {
            if (inv[e] > 0) {
                first_emblem_round = cambc::round_number();
                break;
            }
        }
    }
    if (first_emblem_round != 0 && !early_reported) {
        early_reported = true;
        std::cout << "EARLY gold=" << first_gold_round
                  << " emblem=" << first_emblem_round << '\n';
    }

    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(me.position());
    if (at_spawn) {
        auto& me2 = cambc::self();
        const std::array items{cambc::item_type::power_shard_of_strength,
            cambc::item_type::power_shard_of_focus, cambc::item_type::power_shard_of_health};
        const auto item = items[chosen_stat];
        const unsigned count = me2.gold_count() / item.cost();
        if (count && me2.can_buy(item, count)) me2.buy(item, count);
    }

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

    // Bank any carried gems before farming.
    // Only bank when carrying >= 30 gems (wave banking reduces trips).
    // BISECT: safe-altar routing disabled to isolate the corners shutout.
    if (me.gem_count() >= 30) {
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

    // Cassi & Issac tether (safe form): one draft pick, two bodies, ONE shared
    // death. The old form walked whoever saw the other half back together and
    // hijacked whole turns, which starved farming on big maps. This form never
    // moves or returns: a separated half simply declines to START fights it
    // cannot finish alone, and keeps farming and banking. Separation is still
    // visible in replays as the pair drifting, never as one half diving alone.
    // Returns true when separated (mate unseen or further than 6 tiles).
    auto pair_separated = [&]() -> bool {
        const auto name = me.champion_name();
        const bool is_cassi = (name == cambc::champion_type::cassi);
        const bool is_issac = (name == cambc::champion_type::issac);
        if (!is_cassi && !is_issac)
            return false;
        const auto want = is_cassi ? cambc::champion_type::issac
                                   : cambc::champion_type::cassi;
        for (const auto& ally : cambc::vision::champions(team::ally)) {
            try {
                if (!ally.is_visible() || !ally.is_alive())
                    continue;
                if (ally.champion_name() != want)
                    continue;
                return distance(me.position(), ally.position()) > 6;
            } catch (...) {
                continue;
            }
        }
        return true; // mate not visible at all: assume separated
    };
    const bool separated = pair_separated();
    if (false) {
        const auto name = me.champion_name();
        const bool is_cassi = (name == cambc::champion_type::cassi);
        const bool is_issac = (name == cambc::champion_type::issac);
        if (is_cassi || is_issac) {
            const auto want = is_cassi ? cambc::champion_type::issac
                                       : cambc::champion_type::cassi;
            bool seen = false;
            vec2 mate_pos{0, 0};
            for (const auto& ally : cambc::vision::champions(team::ally)) {
                try {
                    if (!ally.is_visible() || !ally.is_alive())
                        continue;
                    if (ally.champion_name() != want)
                        continue;
                    seen = true;
                    mate_pos = ally.position();
                } catch (...) {
                    continue;
                }
            }
            if (seen && distance(me.position(), mate_pos) > 3) {
                cambc::bitboard goals;
                goals.clear();
                goals = tiles_around(mate_pos, 1);
                move_towards_closest(goals);
                return;
            }
        }
    }

    // Pop a heal or shield before brawling or farming while not critically hurt.
    if (me.health() < me.max_health() * 0.6f)
        cast_defensive_ability();

    // Brawl: swing at anything in range (plain). A separated Cassi/Issac half
    // sits out: starting a fight alone risks the double death, so it farms
    // and banks instead until the pair is back together.
    if (!separated) {
    for (const auto& enemy : cambc::vision::champions(team::enemy))
        if (attack_if_in_range(enemy)) {
            if (use_attack_reset())
                for (const auto& again : cambc::vision::champions(team::enemy))
                    if (attack_if_in_range(again)) break;
            return;
        }
    }


    // Only chase a distant shooter when already hurt enough to matter.
    farm_camps(false);

    me.set_priority(static_cast<std::int32_t>(1 + random_number(100)));
}