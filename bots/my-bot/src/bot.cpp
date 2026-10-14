// Minimal winning bot: gaurab's 9-0 logic + Monster->Home emblems + ward-first
#include <cambc.h>
#include "bot.h"
#include <algorithm>
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
        // Enemies first. Casting camps-first meant that with a camp adjacent
        // we spent our burst damage and our stuns on monsters while an enemy
        // champion was hitting us, which is most of why we dealt 3,442
        // champion damage while taking 11,579.
        if (ability.can_target_enemies() || ability.can_target_ground()) {
            if (ability.can_target_enemies())
                for (const auto& target : cambc::vision::champions(team::enemy))
                    if (target.is_visible() && target.health() > 0
                        && ability.valid_tiles().is_set(target.position()) && ability.use(target))
                        return true;
            if (ability.can_target_ground())
                for (const auto& target : cambc::vision::champions(team::enemy))
                    if (target.is_visible() && target.health() > 0
                        && ability.valid_tiles().is_set(target.position())
                        && ability.use(target.position()))
                        return true;
        }
        if (ability.can_target_camps()) {
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

// Worth walking to unless we can see it respawning. Groups out of vision are
// assumed alive; walking over is how we find out.
bool group_farmable(const cambc::map::camp_group& group, const cambc::bitboard& visible) {
    if ((group.spawn_tiles() & visible).is_empty())
        return true;
    try {
        return !std::holds_alternative<cambc::map::camp_group_respawning>(group.state());
    } catch (...) {
        return true;
    }
}

// Elk and Monolith hit for 60 and 200 a turn. Committing to the nearest group
// regardless of weight walked us into them and stayed there: 40 camp deaths on
// rows. Only take a heavy group when no light one is left to farm.
bool group_is_heavy(const cambc::map::camp_group& group) {
    for (const auto& camp : group.camps())
        if (camp.is_elk() || camp.is_monolith())
            return true;
    return false;
}

// Shared memory, 8 bytes, team-private:
//   0..1  round number (u16) - stale pings can be spotted and ignored
//   2..3  alert tile x (i16, 30000 = none)
//   4..5  alert tile y (i16)
//   6     severity (0 none, 1 enemy seen, 2 ally in trouble)
//   7     reserved
// Vision is not shared between champions, so this is the only way to tell a
// team-mate that trouble is near them.
struct TeamSignal {
    std::uint16_t round;
    std::int16_t x;
    std::int16_t y;
    std::uint8_t severity;
    std::uint8_t spare;
};

constexpr std::int16_t kNoTile = 30000;

TeamSignal read_signal() {
    TeamSignal s{};
    try {
        cambc::shared_memory.read(0, std::span{&s, std::size_t{1}});
    } catch (...) {
    }
    return s;
}

void write_signal(const TeamSignal& s) {
    try {
        cambc::shared_memory.write(0, std::span{&s, std::size_t{1}});
    } catch (...) {
    }
}

} // namespace

void Bot::update_signal() {
    auto& me = cambc::self();
    const auto now = static_cast<std::uint16_t>(cambc::round_number());
    auto sig = read_signal();

    // Raise an alert when enemies are close to an ally, or when we are hurt
    // and being hit. Allies act on it; we never chase it ourselves.
    std::int16_t ax = kNoTile, ay = kNoTile;
    std::uint8_t sev = 0;
    for (const auto& enemy : cambc::vision::champions(team::enemy)) {
        if (!enemy.is_visible() || enemy.health() <= 0)
            continue;
        for (const auto& ally : cambc::vision::champions(team::ally)) {
            if (ally.position().x == me.position().x
                && ally.position().y == me.position().y)
                continue;
            if (!ally.is_visible() || !ally.is_alive())
                continue;
            if (distance(enemy.position(), ally.position()) <= 4) {
                ax = static_cast<std::int16_t>(ally.position().x);
                ay = static_cast<std::int16_t>(ally.position().y);
                sev = 2;
            }
        }
    }
    if (sev == 0 && me.health() < me.max_health() * 0.35f) {
        for (const auto& enemy : cambc::vision::champions(team::enemy)) {
            if (enemy.is_visible() && enemy.health() > 0
                && distance(me.position(), enemy.position()) <= 5) {
                ax = static_cast<std::int16_t>(me.position().x);
                ay = static_cast<std::int16_t>(me.position().y);
                sev = 2;
                break;
            }
        }
    }

    // Only write on change or refresh periodically, so champions covering
    // different parts of the map do not fight over the same bytes.
    const bool changed = sev != sig.severity
        || (sev != 0 && (sig.x != ax || sig.y != ay));
    if (changed || sig.round + 10 < now) {
        sig.round = now;
        sig.x = ax;
        sig.y = ay;
        sig.severity = sev;
        write_signal(sig);
    }
}

// Should this champion break off to defend a team-mate in trouble?
bool Bot::should_help() {
    auto& me = cambc::self();
    const auto sig = read_signal();
    if (sig.severity < 2 || sig.x == kNoTile)
        return false;
    if (static_cast<std::int16_t>(cambc::round_number() - sig.round) > 4)
        return false; // stale ping
    // Never abandon a bag of gems, and never abandon a camp we are clearing:
    // both cost more than the rescue is worth.
    if (me.gem_count() > 0)
        return false;
    if (!cambc::vision::camps().empty())
        return false;

    const vec2 mate{sig.x, sig.y};
    if (distance(me.position(), mate) > 6)
        return false;

    // Only the CLOSEST ally answers a ping. Answering together sent all six
    // champions into one fight: in the ladder match our living-champion count
    // swung between 6 and 0 and we were wiped completely at round 600.
    const int mine = distance(me.position(), mate);
    for (const auto& ally : cambc::vision::champions(team::ally)) {
        if (ally.position().x == me.position().x
            && ally.position().y == me.position().y)
            continue;
        if (!ally.is_visible() || !ally.is_alive())
            continue;
        const int theirs = distance(ally.position(), mate);
        if (theirs < mine)
            return false;
        if (theirs == mine
            && static_cast<int>(ally.champion_name())
                < static_cast<int>(me.champion_name()))
            return false;
    }
    return true;
}


// Place the free ward where it tells us the most. One active ward, replaceable
// once per round, 6 tiles of permanent vision. Vision is not shared, so every
// champion needs its own: watching the altar we are about to bank at spots
// enemy locks before we commit, and watching our camp tells us it respawned
// without walking over. The opponent ran 5 active wards in the loss match; we
// ran none.
void Bot::maybe_place_ward() {
    auto& me = cambc::self();
    if (me.ward_cooldown() != 0 || !me.can_move())
        return;
    auto placement = me.ward_placement_tiles();
    if (placement.is_empty())
        return;

    cambc::bitboard want;
    want.clear();

    // 1. The altar we are carrying gems to: watch the approach for locks.
    for (auto altar : cambc::map::altars().positions())
        if (me.gem_count() > 0 && !cambc::vision::has_altar_lock(altar)) {
            want = tiles_around(altar, cambc::constants::submit_gems_range);
            break;
        }
    // 2. Otherwise the camp we are working, so we can see it respawn.
    if (want.is_empty() && !farm_target.is_empty())
        want = farm_target;
    // 3. Otherwise the nearest altar: covers the team objective.
    if (want.is_empty()) {
        vec2 best{0, 0};
        int best_dist = 1 << 20;
        for (auto altar : cambc::map::altars().positions()) {
            const int d = distance(me.position(), altar);
            if (d < best_dist) {
                best_dist = d;
                best = altar;
            }
        }
        if (best_dist != 1 << 20)
            want = tiles_around(best, 2);
    }
    if (want.is_empty())
        return;

    auto tiles = want & placement;
    if (tiles.is_empty())
        tiles = want;
    if (tiles.is_empty())
        return;
    try {
        // Push the ward as far from us as the placement mask allows: it
        // outlives us, so distance from our current tile is free range.
        auto opts = tiles.positions();
        vec2 best = opts[0];
        int best_score = distance(me.position(), opts[0]);
        for (auto tile : opts) {
            const int d = distance(me.position(), tile);
            if (d > best_score) {
                best_score = d;
                best = tile;
            }
        }
        me.place_ward(best);
    } catch (...) {
    }
}

// Dash out of danger. Vendetta, Vesper Vale, Ren and Issac all carry a
// two-tile dash on an 8-turn cooldown; spending it while a camp is beating on
// us buys two free tiles before the walk-away, which is the difference
// between escaping and feeding the bag. The dash crosses holes but not walls,
// so we only consider tiles the engine already offers us.
bool Bot::dash_escape() {
    auto& me = cambc::self();
    if (!me.can_cast())
        return false;

    cambc::bitboard danger;
    danger.clear();
    for (const auto& camp : cambc::vision::camps()) {
        if (camp.health() > 0)
            danger.draw_chebyshev(camp.position(), camp.attack_range());
    }
    for (const auto& enemy : cambc::vision::champions(team::enemy)) {
        if (enemy.is_visible() && enemy.health() > 0)
            danger.draw_chebyshev(enemy.position(), enemy.attack_range() + 1);
    }

    for (const auto& ab : me.abilities()) {
        if (!ab.is_dash() || !ab.is_ready())
            continue;
        cambc::bitboard tiles;
        try {
            tiles = ab.valid_tiles();
        } catch (...) {
            continue;
        }
        if (tiles.is_empty())
            continue;
        // Somewhere to land that nothing can reach next turn.
        auto safe = tiles & ~danger;
        if (safe.is_empty())
            return false; // no escape: do not burn the cooldown for nothing
        auto opts = safe.positions();
        // Farthest from the nearest threat is the best run.
        auto best = opts[0];
        int best_score = -1;
        for (auto tile : opts) {
            int score = 1 << 20;
            for (const auto& camp : cambc::vision::camps()) {
                if (camp.health() <= 0)
                    continue;
                score = std::min(score, distance(tile, camp.position()));
            }
            for (const auto& enemy : cambc::vision::champions(team::enemy)) {
                if (!enemy.is_visible() || enemy.health() <= 0)
                    continue;
                score = std::min(score, distance(tile, enemy.position()));
            }
            if (score > best_score) {
                best_score = score;
                best = tile;
            }
        }
        try {
            return ab.use(best);
        } catch (...) {
            return false;
        }
    }
    return false;
}

cambc::bitboard Bot::farm_goal(bool& committed) {
    const auto visible = cambc::vision::visible_tiles();
    const auto& groups = cambc::map::camp_groups();

    // Re-pick the best group every turn rather than holding one until it
    // dies. Holding it stopped the oscillation the user first reported, but it
    // also froze farming onto a stale target: against hist-surv our monster
    // damage was 20,607 against their 69,442. Re-picking gives 26,498 and two
    // fewer deaths. The altar commitment stays, because that is what fixed
    // banking.
    committed = false;

    // Distance dominates
    // because travel is what actually limits clears; gem value only breaks
    // ties. Light groups are tried first so we never lock onto an Elk or
    // Monolith group while softer camps are available.
    const auto me = cambc::self().position();
    const cambc::map::camp_group* best = nullptr;
    long best_key = 0;
    for (int pass = 0; pass < 2 && best == nullptr; ++pass) {
        for (const auto& group : groups) {
            if (!group_farmable(group, visible))
                continue;
            if (pass == 0 && group_is_heavy(group))
                continue;
            int dist = 1 << 20;
            for (auto tile : group.spawn_tiles().positions())
                dist = std::min(dist, distance(me, tile));
            const long key = static_cast<long>(dist) * 100
                - static_cast<long>(group.gem_value());
            if (best == nullptr || key < best_key) {
                best = &group;
                best_key = key;
            }
        }
    }
    if (best == nullptr) {
        farm_target.clear();
        return farm_target;
    }
    farm_target = best->spawn_tiles();
    committed = true;
    return farm_target;
}

void Bot::submit_if_at_altar() {
    auto& me = cambc::self();
    for (auto altar : cambc::map::altars().positions()) {
        if (distance(me.position(), altar) > cambc::constants::submit_gems_range)
            continue;
        if (cambc::vision::has_altar_lock(altar))
            continue;
        try {
            me.submit_gems(altar);
        } catch (...) {
        }
        return;
    }
}

// Commit to one unlocked altar and walk to it. Re-picking every turn made a
// courier pace between two equidistant altars and never bank anything.
cambc::bitboard Bot::bank_goal(bool& committed) {
    const auto me = cambc::self().position();

    if (committed) {
        for (auto altar : cambc::map::altars().positions()) {
            if (!bank_target.is_set(altar))
                continue;
            if (cambc::vision::has_altar_lock(altar))
                break;
            return tiles_around(altar, cambc::constants::submit_gems_range);
        }
        committed = false;
        bank_target.clear();
    }

    const auto blocked = blocked_tiles();
    vec2 best_altar{0, 0};
    int best_dist = 1 << 20;
    for (auto altar : cambc::map::altars().positions()) {
        if (cambc::vision::has_altar_lock(altar))
            continue;
        const int d = distance(me, altar);
        if (d < best_dist) {
            best_dist = d;
            best_altar = altar;
        }
    }
    if (best_dist == 1 << 20) {
        bank_target.clear();
        return bank_target;
    }

    bank_target.clear();
    bank_target.set(best_altar);
    committed = true;
    auto goals = tiles_around(best_altar, cambc::constants::submit_gems_range);
    goals &= ~blocked | blocked;
    return goals;
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
        bool committed = !farm_target.is_empty();
        goals = farm_goal(committed);
        // Nothing worth walking to: hold position rather than pace.
        if (goals.is_empty())
            return;
    }

    // Re-check camps after moving, but never cast there: "excludes walking"
    // spells throw once we have moved (engine error 126) and waste the turn.
    if (move_towards_closest(goals))
        attack_camps();
}

// DIAGNOSTIC: which state absorbs each turn? Printed every 200 rounds.
// This is what revealed that we farm 0% of turns while hist-surv farms
// 53-80%. Cheap: only runs once every 200 rounds.
namespace {
struct Tally {
    long dead = 0, recall = 0, channel = 0, stun = 0;
    long bank = 0, flee = 0, fight = 0, help = 0, farm = 0, bankmoving = 0;
};
Tally tally;
void report_tally(std::uint32_t round) {
    const long total = tally.dead + tally.recall + tally.channel + tally.stun
        + tally.bank + tally.flee + tally.fight + tally.help + tally.farm;
    std::cout << "TALLY r=" << round << " tot=" << total
        << " dead=" << tally.dead << " recall=" << tally.recall
        << " chan=" << tally.channel << " stun=" << tally.stun
        << " bank=" << tally.bank << " flee=" << tally.flee
        << " fight=" << tally.fight << " help=" << tally.help
        << " farm=" << tally.farm << " bankmoving=" << tally.bankmoving << '\n';
}
}

void Bot::play_turn() {
    static std::uint32_t last_report = 0;
    if (cambc::round_number() >= last_report + 200) {
        last_report = cambc::round_number();
        report_tally(last_report);
        std::cout.flush();
    }

    // Initialise build: power shards of strength scale our basic attacks.
    // Small drafts (≤3) fight short, burst-heavy skirmishes -> stack strength.
    // Larger drafts are attrition fights -> health shards keep champs alive.
    if (chosen_stat < 0) {
        chosen_stat = cambc::draft::team_size() <= 3 ? 0 : 2;
    }

    auto& me = cambc::self();

    // Publish danger for team-mates before acting on anything.
    update_signal();
    maybe_place_ward();

    if (!me.is_alive()) { ++tally.dead; return; }
    if (me.is_recalling()) { ++tally.recall; return; }
    if (me.is_submitting_gems() || me.is_teleporting()) { ++tally.channel; return; }
    if (!me.can_move() && !me.can_attack()) { ++tally.stun; return; }

    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(me.position());
    if (at_spawn) {
        auto& me2 = cambc::self();
        // Monster, not Power: the emblem passive (+10% damage to camps) makes
        // every later clear faster, which compounds into more captures per
        // round. Power's passive is "None; one extra shard's worth" - flat
        // stats only, and its emblem needs 6 shards instead of 5.
        const std::array items{cambc::item_type::monster_shard_of_strength,
            cambc::item_type::monster_shard_of_focus,
            cambc::item_type::monster_shard_of_health};
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
            ++tally.flee;
            if (under_threat()) {
                // Two free tiles first if this champion has a dash.
                if (!dash_escape())
                    move_towards_closest(cambc::map::spawn_region(team::ally));
            } else {
                me.recall();
            }
            return;
        }
    }

    // Pop a heal or shield before brawling or farming while not critically hurt.
    if (me.health() < me.max_health() * 0.6f)
        cast_defensive_ability();

    // Bank any carried gems before farming. Carried gems score nothing, so
    // this must come before anything that would walk us somewhere else.
    if (me.gem_count() > 0) {
        ++tally.bank;
        bool committed = !bank_target.is_empty();
        const auto goals = bank_goal(committed);
        if (!goals.is_empty()) {
            if (move_towards_closest(goals)) {
                ++tally.bankmoving;
                submit_if_at_altar();
            }
        }
        return;
    }
    bank_target.clear();

    // Brawl: swing at an adjacent enemy and cash attack-reset for second swing.
    for (const auto& enemy : cambc::vision::champions(team::enemy))
        if (attack_if_in_range(enemy)) {
            ++tally.fight;
            if (use_attack_reset())
                for (const auto& again : cambc::vision::champions(team::enemy))
                    if (attack_if_in_range(again)) break;
            return;
        }

    // Help a team-mate flagged in shared memory. Only when we carry no gems:
    // a lost bag costs the whole trip.
    if (should_help()) {
        ++tally.help;
        const auto sig = read_signal();
        const vec2 spot{sig.x, sig.y};
        if (distance(me.position(), spot) <= me.attack_range() + 2) {
            for (const auto& enemy : cambc::vision::champions(team::enemy))
                if (attack_if_in_range(enemy))
                    return;
        } else if (!move_towards_closest(tiles_around(spot, 1))) {
            farm_camps(false);
            return;
        } else {
            cast_offensive_ability();
            for (const auto& enemy : cambc::vision::champions(team::enemy))
                if (attack_if_in_range(enemy))
                    return;
        }
        return;
    }
    ++tally.farm;
    farm_camps(false);

    me.set_priority(static_cast<std::int32_t>(1 + random_number(100)));
}