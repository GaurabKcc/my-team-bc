#pragma once

#include <cambc.h>

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

    // Compact logging state
    int last_gems = -1;
    float last_log_health = 0.0f;
    int last_log_gems = -1;

    // Spend on power shards of the chosen stat, keeping some health for survival.
    void buy_items();

public:
    // Score and run the best action state for this turn.
    void play_turn();
};
