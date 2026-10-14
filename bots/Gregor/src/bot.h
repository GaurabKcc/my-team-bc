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

    // Telemetry: emit TEAM and per-champion event lines. No gameplay effect.
    void log_telemetry(bool is_designated);
    // Spend on power shards of the chosen stat, keeping some health for survival.
    void buy_items();

public:
    // Score and run the best action state for this turn.
    void play_turn();
};
