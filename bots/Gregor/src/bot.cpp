// Champion decisions. main.cpp dispatches here after the draft.
#include <cambc.h>
#include "bot.h"
#include <array>
#include <iostream>
#include "random.h"
#include <cstdint>
#include <span>
#include <string>
#include <type_traits>
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

// Telemetry only: the tile the last move_towards_closest aimed for.
namespace { cambc::vec2 g_move_target{}; bool g_move_target_valid = false; }

bool move_towards_closest(cambc::bitboard goals, const cambc::bitboard& avoid = cambc::bitboard{}) {
    auto& me = cambc::self();
    if (!me.can_move() || goals.is_set(me.position()))
        return false;

    // Only free goals or occupied camp tiles can seed the field. `avoid` tiles
    // (camp danger) are routed around, but goal tiles stay reachable.
    auto blocked = blocked_tiles();
    blocked |= avoid & ~goals;
    goals &= ~blocked | cambc::vision::camp_tiles();
    if (goals.is_empty())
        return false;
    if (goals.count() == 1 && !blocked.is_set(goals.positions()[0])) {
        g_move_target = goals.positions()[0];
        g_move_target_valid = true;
        return safely([&] { return me.move_towards(goals.positions()[0]); });
    }

    // Expand from the goals until our current tile is reached.
    const auto path_walls = blocked & ~goals;
    cambc::distance_field field;
    cambc::bitboard visited;
    field.compute_to(path_walls, goals, tiles_around(me.position(), 0), visited);

    // Choose a reachable step with a smaller distance to the goal.
    auto step = me.position();
    for (auto tile : blocked.reachable_tiles(step, me.base_stats().base_move_speed).positions())
        if (field.get(tile) < field.get(step)) step = tile;
    if (step != me.position()) {
        g_move_target = step;
        g_move_target_valid = true;
        return safely([&] { return me.move_absolute(step); });
    }

    // Local minimum (e.g. a goal boxed in by terrain): nudge greedily toward the
    // nearest goal rather than idling in place for the rest of the game.
    vec2 nearest = goals.positions()[0];
    for (auto tile : goals.positions())
        if (field.get(tile) < field.get(nearest)) nearest = tile;
    g_move_target = nearest;
    g_move_target_valid = true;
    return safely([&] { return me.move_towards(nearest); });
}

// ---- Threat model and disengage (Prompt 2). Camps do not chase, so a zone is
// only lethal while the camp is awake; a wounded visible camp is treated as
// awake. Routing uses the static union of every camp's attack zone. ----

namespace {

// Design attack range and strength per camp type (constant for the match).
struct camp_design { int range; float strength; };

const std::array<camp_design, 5>& camp_type_design() {
    static const std::array<camp_design, 5> design = [] {
        std::array<camp_design, 5> out{};
        for (const auto& group : cambc::map::camp_groups())
            for (const auto& member : group) {
                const int idx = static_cast<int>(member.kind());
                if (idx < 0 || idx >= static_cast<int>(out.size())) continue;
                const auto bs = member.base_stats();
                out[idx] = { bs.base_attack_range, bs.base_strength };
            }
        return out;
    }();
    return design;
}

// Union of every camp's attack zone; used to route around danger statically.
const cambc::bitboard& all_danger_tiles() {
    static const cambc::bitboard danger = [] {
        cambc::bitboard board;
        board.clear();
        const auto& design = camp_type_design();
        for (const auto& group : cambc::map::camp_groups())
            for (const auto& member : group) {
                const int idx = static_cast<int>(member.kind());
                if (idx < 0 || idx >= static_cast<int>(design.size())) continue;
                if (design[idx].range > 0)
                    board.draw_chebyshev(member.spawn_position(), design[idx].range);
            }
        return board;
    }();
    return danger;
}

// Expected incoming camp damage per round on a tile: strength of each visible,
// wounded (awake) camp whose attack zone covers the tile.
float incoming_damage_at(cambc::vec2 tile) {
    float total = 0.0f;
    const auto& design = camp_type_design();
    for (const auto& camp : cambc::vision::camps()) {
        bool usable = false;
        safely([&] { usable = camp.is_visible() && camp.health() > 0; return usable; }, "threat_vis");
        if (!usable) continue;
        float hp = 0.0f, max_hp = 0.0f;
        safely([&] { hp = camp.health(); max_hp = camp.max_health(); return true; }, "threat_hp");
        if (hp >= max_hp) continue; // unhurt camp is asleep
        const int idx = static_cast<int>(camp.kind());
        if (idx < 0 || idx >= static_cast<int>(design.size()) || design[idx].range <= 0) continue;
        if (distance(tile, camp.position()) <= design[idx].range)
            total += design[idx].strength;
    }
    return total;
}

bool at_own_spawn() {
    return cambc::map::spawn_region(cambc::team::ally).is_set(cambc::self().position());
}

// Move to the reachable tile furthest from camp danger, preferring tiles toward
// the nearest unlocked altar (when carrying gems) or the spawn region. Returns
// true if it actually moved.
bool move_to_safe_tile() {
    auto& me = cambc::self();
    if (!me.can_move()) return false;
    const auto& danger = all_danger_tiles();
    if (!danger.is_set(me.position())) return false;

    const auto blocked = blocked_tiles();
    cambc::bitboard reachable;
    reachable.clear();
    blocked.reachable_tiles_into(me.position(),
        static_cast<std::uint16_t>(me.base_stats().base_move_speed), reachable);
    reachable.set(me.position(), false);
    if (reachable.is_empty()) return false;

    // Preference point: nearest unlocked altar when carrying gems, else spawn.
    cambc::vec2 prefer = me.position();
    bool have_prefer = false;
    if (me.gem_count() > 0) {
        int best = 1 << 30;
        for (auto altar : cambc::map::altars().positions()) {
            if (cambc::vision::has_altar_lock(altar)) continue;
            const int d = distance(me.position(), altar);
            if (d < best) { best = d; prefer = altar; have_prefer = true; }
        }
    }
    if (!have_prefer) {
        int best = 1 << 30;
        for (auto spawn : cambc::map::spawn_region(cambc::team::ally).positions()) {
            const int d = distance(me.position(), spawn);
            if (d < best) { best = d; prefer = spawn; have_prefer = true; }
        }
    }

    cambc::vec2 best_tile = me.position();
    bool found = false;
    int best_pref = 1 << 30;
    for (auto tile : reachable.positions()) {
        if (danger.is_set(tile)) continue; // step outside every zone first
        const int pref = distance(tile, prefer);
        if (!found || pref < best_pref) { best_tile = tile; best_pref = pref; found = true; }
    }
    if (!found) {
        // Boxed in: take the least-damaging reachable tile, then closest home.
        float best_dmg = incoming_damage_at(me.position());
        for (auto tile : reachable.positions()) {
            const float dmg = incoming_damage_at(tile);
            const int pref = distance(tile, prefer);
            if (dmg < best_dmg || (dmg == best_dmg && pref < best_pref)) {
                best_tile = tile; best_dmg = dmg; best_pref = pref; found = true;
            }
        }
    }
    if (found && best_tile != me.position())
        return safely([&] { return me.move_absolute(best_tile); }, "survival_move");
    return false;
}

// Whether an awake camp sits inside our own attack range, so standing ground
// means we are trading blows rather than being shot for free.
bool can_hit_camp_back() {
    auto& me = cambc::self();
    const int range = me.attack_range();
    for (const auto& camp : cambc::vision::camps()) {
        bool usable = false;
        safely([&] { usable = camp.is_visible() && camp.health() > 0; return usable; }, "retaliate_vis");
        if (!usable) continue;
        float hp = 0.0f, max_hp = 0.0f;
        safely([&] { hp = camp.health(); max_hp = camp.max_health(); return true; }, "retaliate_hp");
        if (hp >= max_hp) continue; // asleep camp
        if (distance(me.position(), camp.position()) <= range) return true;
    }
    return false;
}

// Disengage before dropping to the old 35% recall trigger. `took_damage` is true
// when our health dropped since last turn, which also catches hidden attackers.
// Returns true if the turn was spent fleeing or recalling.
bool try_disengage_or_recall(bool took_damage) {
    auto& me = cambc::self();
    if (at_own_spawn()) return false;
    const float max_hp = me.max_health();
    const float hp = me.health();
    // A healthy champion pays nothing: no camp queries until actually hurt.
    if (hp >= max_hp * 0.5f) return false;
    const float incoming = incoming_damage_at(me.position());
    const bool being_hit = took_damage || incoming > 0.0f;

    // Out-ranged while hurt: back off instead of trading on the spot.
    if (hp < max_hp * 0.5f && incoming > 0.0f && !can_hit_camp_back()) {
        if (move_to_safe_tile()) return true;
    }

    // Recall once safely clear of camp danger; never channel while being hit.
    const bool critical = hp < max_hp * 0.35f;
    // A lone wolf/elk is survivable at 35%; a monolith's 200/round is not, so
    // break off early only when the incoming rate is genuinely lethal.
    const bool lethal = incoming > 0.0f && hp < incoming * 1.5f + max_hp * 0.10f;
    if (!critical && !lethal) return false;

    if (being_hit) {
        // A recall started now would just be cancelled by the next hit.
        if (move_to_safe_tile()) return true;
        return false; // cornered: stay and fight rather than waste the channel
    }
    if (safely([&] { me.recall(); return true; }, "survival_recall"))
        return true;
    return false;
}

} // namespace

void Bot::buy_items() {
    auto& me = cambc::self();
    const std::array items{cambc::item_type::power_shard_of_strength,
        cambc::item_type::power_shard_of_focus, cambc::item_type::power_shard_of_health};
    const auto item = items[chosen_stat];
    const unsigned count = me.gold_count() / item.cost();
    if (count && me.can_buy(item, count)) {
        const std::uint32_t gold = me.gold_count();
        if (safely([&] { me.buy(item, count); return true; }, "buy")) {
            std::cout << "E|r=" << cambc::round_number()
                      << "|cid=" << static_cast<std::uint32_t>(static_cast<cambc::handle_champion_t>(me))
                      << "|BUY|item=" << cambc::to_string(item) << "|n=" << count
                      << "|spent=" << static_cast<std::uint32_t>(item.cost() * count)
                      << "|gold=" << static_cast<std::uint32_t>(gold - item.cost() * count) << '\n';
        }
    }
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

// ---- Telemetry (Prompt 1). Read-only instrumentation: event lines plus one
// throttled TEAM summary per team. It must not change gameplay. ----

namespace {

std::uint32_t self_id(const cambc::champion& me) {
    return static_cast<std::uint32_t>(static_cast<cambc::handle_champion_t>(me));
}

std::string nearest_camp_name(vec2 pos) {
    std::string best = "none";
    int best_d = 1 << 30;
    for (const auto& camp : cambc::vision::camps()) {
        bool usable = false;
        safely([&] { usable = camp.is_visible(); return usable; }, "tele_camp");
        if (!usable) continue;
        const int d = distance(pos, camp.position());
        if (d < best_d) {
            best_d = d;
            best = cambc::to_string(camp.kind());
        }
    }
    return best;
}

// All camp-group spawn tiles for the match; static so it is only computed once.
const cambc::bitboard& all_camp_spawn_tiles() {
    static const cambc::bitboard tiles = [] {
        cambc::bitboard result;
        result.clear();
        for (const auto& group : cambc::map::camp_groups())
            result |= group.spawn_tiles();
        return result;
    }();
    return tiles;
}

} // namespace

void Bot::log_telemetry(bool is_designated) {
    auto& me = cambc::self();
    const std::uint32_t round = cambc::round_number();
    const std::uint32_t cid = self_id(me);

    // ---- Death / respawn: the only per-turn query every champion needs. ----
    bool alive = true;
    safely([&] { alive = me.is_alive(); return alive; }, "tele_alive");
    if (last_alive && !alive) {
        std::cout << "E|r=" << round << "|cid=" << cid << "|DEATH|hp=0|gems="
                  << last_gem_count << "|near=" << nearest_camp_name(last_position) << '\n';
    } else if (!last_alive && alive) {
        std::cout << "E|r=" << round << "|cid=" << cid << "|RESPAWN|hp="
                  << static_cast<int>(me.health()) << "|gems=" << me.gem_count()
                  << "|near=" << nearest_camp_name(me.position()) << '\n';
    }
    last_alive = alive;
    if (!alive) {
        last_recalling = false;
        last_submitting = false;
        return;
    }

    // ---- Tribute start / completion (only on transitions). ----
    bool submitting = false;
    safely([&] { submitting = me.is_submitting_gems(); return submitting; }, "tele_submit");
    if (submitting != last_submitting) {
        if (submitting) {
            tribute_start_gems = me.gem_count();
            std::cout << "E|r=" << round << "|cid=" << cid << "|TRIBUTE_START|gems="
                      << tribute_start_gems << '\n';
        } else {
            // A completed tribute consumes all carried gems; a cancelled one keeps
            // them, so gem_count()==0 distinguishes completion.
            const std::uint32_t gem_now = me.gem_count();
            if (gem_now == 0 && tribute_start_gems > 0) {
                std::uint32_t ally_submitted = 0;
                safely([&] { ally_submitted = cambc::global_submitted_gems(team::ally); return true; }, "tele_submitted");
                last_submitted = ally_submitted;
                std::cout << "E|r=" << round << "|cid=" << cid << "|TRIBUTE_DONE|my="
                          << ally_submitted << "|carried=" << tribute_start_gems << '\n';
            } else {
                std::cout << "E|r=" << round << "|cid=" << cid << "|TRIBUTE_CANCEL|gems="
                          << gem_now << '\n';
            }
        }
    }
    last_submitting = submitting;

    // ---- Recall start / cancellation (only on transitions). ----
    bool recalling = false;
    safely([&] { recalling = me.is_recalling(); return recalling; }, "tele_recall");
    if (recalling != last_recalling) {
        if (recalling) {
            std::cout << "E|r=" << round << "|cid=" << cid << "|RECALL_START|hp="
                      << static_cast<int>(me.health()) << '\n';
        } else if (me.health() < last_health) {
            std::cout << "E|r=" << round << "|cid=" << cid << "|RECALL_CANCEL|hp="
                      << static_cast<int>(me.health()) << "|from=" << static_cast<int>(last_health) << '\n';
        }
    }
    last_recalling = recalling;

    // ---- First sight of enemies: only until every distinct enemy is known. ----
    if (seen_enemy_count < seen_enemy_handles.size()) {
        for (const auto& enemy : cambc::vision::champions(team::enemy)) {
            bool usable = false;
            safely([&] { usable = enemy.is_visible() && enemy.health() > 0; return usable; }, "tele_enemy");
            if (!usable) continue;
            const std::uint32_t h = self_id(enemy);
            bool seen = false;
            for (unsigned i = 0; i < seen_enemy_count; ++i)
                if (seen_enemy_handles[i] == h) { seen = true; break; }
            if (!seen && seen_enemy_count < seen_enemy_handles.size()) {
                seen_enemy_handles[seen_enemy_count++] = h;
                std::cout << "E|r=" << round << "|cid=" << cid << "|ENEMY_SEEN|type="
                          << cambc::to_string(enemy.champion_name()) << "|pos="
                          << cambc::to_string(enemy.position()) << "|hp="
                          << static_cast<int>(enemy.health()) << '\n';
            }
        }
    }

    // ---- Acceptance metric: standing still while damaged and at low health,
    // and either unable to hit a camp back or stuck in a recall channel. Active
    // trades (camp in our own attack range) are intentional, not violations. ----
    const bool idle_low = me.health() < me.max_health() * 0.5f && !can_hit_camp_back();
    if (has_memory && distance(me.position(), last_position) == 0
        && me.health() < last_health && (idle_low || recalling)) {
        ++danger_still_turns;
        if (danger_still_turns == 2)
            std::cout << "E|r=" << round << "|cid=" << cid << "|STUCK_DANGER|hp="
                      << static_cast<int>(me.health()) << "|incoming="
                      << static_cast<int>(incoming_damage_at(me.position())) << '\n';
    } else {
        danger_still_turns = 0;
    }

    // The remaining telemetry is more expensive; only the designated champion
    // pays it so the per-champion fuel cost of telemetry stays small.
    if (!is_designated) {
        last_gem_count = me.gem_count();
        return;
    }

    // ---- Capture start: a group's reward becomes available while we stand on
    // its spawn tile. Gated on the cached global spawn mask; camp_groups() is
    // only queried when actually standing on a camp spawn. ----
    if (all_camp_spawn_tiles().is_set(me.position())) {
        const auto groups = cambc::map::camp_groups();
        for (std::size_t i = 0; i < groups.size() && i < group_last_state.size(); ++i) {
            const auto& group = groups[i];
            if (!group.spawn_tiles().is_set(me.position())) continue;
            std::int8_t st = 0;
            safely([&] {
                std::visit([&](auto&& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, cambc::map::camp_group_alive>) st = 1;
                    else if constexpr (std::is_same_v<T, cambc::map::camp_group_reward_available>) st = 2;
                    else st = 3;
                }, group.state());
                return true;
            }, "tele_capture");
            if (st == 2 && group_last_state[i] != 2)
                std::cout << "E|r=" << round << "|cid=" << cid << "|CAPTURE_START|gems="
                          << group.gem_value() << "|gold=" << group.gold_value() << '\n';
            group_last_state[i] = st;
        }
    }

    // ---- One throttled TEAM summary, emitted by the designated champion. ----
    if (round >= last_team_round + 10) {
        last_team_round = round;
        int alive_ally = 1;
        std::uint32_t carry = me.gem_count();
        for (const auto& ally : cambc::vision::champions(team::ally)) {
            if (is_self(ally)) continue;
            bool usable = false;
            safely([&] { usable = ally.is_visible() && ally.health() > 0; return usable; }, "tele_ally");
            if (usable) { ++alive_ally; carry += ally.gem_count(); }
        }
        std::uint32_t my_submitted = 0, opp_submitted = 0;
        safely([&] { my_submitted = cambc::global_submitted_gems(team::ally); return true; }, "tele_my");
        safely([&] { opp_submitted = cambc::global_submitted_gems(team::enemy); return true; }, "tele_opp");
        last_submitted = my_submitted;
        std::cout << "T|r=" << round << "|my=" << my_submitted << "|opp=" << opp_submitted
                  << "|alive=" << alive_ally << "|carry=" << carry
                  << "|gold=" << me.gold_count() << '\n';
    }

    // ---- Overlays: altar goal (green), nearest camp (red), move step (blue). ----
    for (auto altar : cambc::map::altars().positions()) {
        if (!cambc::vision::has_altar_lock(altar)) {
            safely([&] { cambc::debug::draw_dot(altar, 0x00C000); return true; }, "tele_dot_altar");
            break;
        }
    }
    for (const auto& camp : cambc::vision::camps()) {
        bool usable = false;
        safely([&] { usable = camp.is_visible(); return usable; }, "tele_dot_camp");
        if (usable) {
            safely([&] { cambc::debug::draw_dot(camp.position(), 0xC00000); return true; }, "tele_dot_camp");
            break;
        }
    }
    if (g_move_target_valid) {
        safely([&] {
            cambc::debug::draw_line(me.position(), g_move_target, 0x0000C0);
            return true;
        }, "tele_draw_line");
        g_move_target_valid = false;
    }

    last_gem_count = me.gem_count();
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
    round = cambc::round_number();
    cambc::shared_memory.write(0, std::span{&round, 1});

    auto& me = cambc::self();

    // Telemetry: the lowest-id living ally emits the throttled TEAM summary.
    // Use the vision-independent roster so exactly one champion is designated.
    bool is_designated = true;
    {
        const std::uint32_t my_id = static_cast<std::uint32_t>(static_cast<cambc::handle_champion_t>(me));
        std::uint32_t min_handle = my_id;
        for (const auto& ally : cambc::champions(team::ally)) {
            bool alive_ally = false;
            safely([&] { alive_ally = ally.is_alive(); return alive_ally; }, "tele_ally_alive");
            if (!alive_ally) continue;
            const std::uint32_t h = static_cast<std::uint32_t>(static_cast<cambc::handle_champion_t>(ally));
            if (h < min_handle) min_handle = h;
        }
        is_designated = (min_handle == my_id);
    }
    log_telemetry(is_designated);

    // Leave channels running and skip turns where we cannot act.
    if (!me.is_alive())
        return;
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting() || (!me.can_move() && !me.can_attack()))
        return;

    // Remember progress: losing health since last turn means something is hitting us.
    const bool took_damage = has_memory && me.health() < last_health;
    last_health = me.health();
    last_position = me.position();
    has_memory = true;

    // Shop before leaving spawn.
    const bool at_spawn = cambc::map::spawn_region(team::ally).is_set(me.position());
    if (at_spawn)
        buy_items();

    // Spawn heals to full at turn end, but only if we stay. Do not step off
    // spawn while wounded, or recalls arrive and are wasted on an unhealed champion.
    if (at_spawn && me.health() < me.max_health())
        return;

    // Disengage from camp danger before we get low enough to die.
    if (try_disengage_or_recall(took_damage))
        return;

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
