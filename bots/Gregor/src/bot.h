#pragma once

#include <cambc.h>
#include <array>
#include <cstdint>

// Per-champion state survives between turns. Pick one stat and keep buying it.
class Bot {
    int chosen_stat = -1;

    // Progress memory for stuck detection and "being shot" retaliation.
    cambc::vec2 last_position;
    float last_health = 0.0f;
    int stuck_turns = 0;
    bool has_memory = false;

    // Turn state to enforce move/cast/attack ordering rules
    bool did_move = false;
    bool did_attack = false;
    bool attack_spent = false;

    // ---- Telemetry state (Prompt 1): event logs only, no per-round spam. ----
    bool last_alive = true;              // detect death / respawn transitions
    bool last_recalling = false;         // detect recall start / cancellation
    bool last_submitting = false;        // detect tribute start / completion
    std::uint32_t last_gem_count = 0;    // bag just before death (for DEATH line)
    std::uint32_t last_submitted = 0;    // ally tributed gems as of last turn
    std::uint32_t tribute_start_gems = 0;      // carried gems when a tribute began
    std::uint32_t last_team_round = 0;   // throttle the TEAM line to every 10 rounds
    // Enemy first-sight memo: handles we have already reported, fixed size so
    // the per-turn path never allocates.
    std::array<std::uint32_t, 16> seen_enemy_handles{};
    unsigned seen_enemy_count = 0;
    // Camp-group telemetry: last state index observed, to detect capture start.
    std::array<std::int8_t, 64> group_last_state{};
    // Acceptance metric: consecutive rounds standing still while damaged and low.
    int danger_still_turns = 0;

    // ---- Camp-selection memory (Prompt 3): camps outside vision are not
    // exposed, so remember the last-seen lifecycle of each group and decay the
    // respawn timer by elapsed rounds. ----
    struct GroupMemory {
        std::uint8_t state = 0;     // 0 unknown, 1 alive, 2 reward, 3 respawning
        std::int32_t timer = 0;     // remaining rounds when last seen
        std::uint32_t round_seen = 0;
    };
    std::array<GroupMemory, 64> group_mem{};

    // ---- Team plan (Prompt 4), mirrored from the 8-byte shared memory. The
    // lowest-id living ally is the coordinator and writes it; everyone reads it
    // and converges on the same target. Layout is documented atop bot.cpp. ----
    int plan_target = -1;    // camp-group index, -1 for none
    int plan_phase = 0;      // 0 idle, 1 gather, 2 engage, 3 capture
    int plan_travel = 0;     // rounds the team needs to reach the target
    int plan_set_round = 0;  // round the target was chosen (low byte)
    int plan_capturer = -1;  // champion id that should claim the reward
    int rally_wait = 0;      // rounds spent regrouping before a solo fight is allowed

    // Telemetry: emit TEAM and per-champion event lines. No gameplay effect.
    void log_telemetry(bool is_designated);
    // Spend on power shards of the chosen stat, keeping some health for survival.
    void buy_items();
    // Refresh last-seen lifecycle state for every camp group currently in vision.
    void update_group_memory();
    // Coordinate the team plan through shared memory. The coordinator picks the
    // focus target and its phase; every champion reads the plan back.
    void refresh_team_plan(bool is_designated);
    // Score every camp group by reward against kill time and risk, refuse the
    // bad ones (monolith, clearly lethal, respawning) and return the spawn tiles
    // of all that remain.
    cambc::bitboard choose_camp_goals();
    // Attack a camp in range, otherwise approach the best-scoring camp group.
    void farm_camps(bool engage_shooter, bool did_move_in, bool& attack_spent_out);

public:
    // Score and run the best action state for this turn.
    void play_turn();
};
