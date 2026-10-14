#pragma once

#include <cambc.h>
#include <cstdint>

class Bot {
    int chosen_stat = -1;
    // Early-game timing (diagnostic): first round this champion holds any
    // gold (proxy for first capture) and first round it owns any emblem.
    // Reported once so replays carry opening-speed data.
    std::uint32_t first_gold_round = 0;
    std::uint32_t first_emblem_round = 0;
    bool early_reported = false;
    // Sector assignment from the draft plan. Each champion owns a different
    // camp group so the team spreads instead of converging on one camp.
    bool assignment_read = false;
    int my_group = -1;
    // Pack from the draft plan (0 or 1). Packmates share one camp group, so
    // the pack converges for fast clears while the two packs work in
    // parallel. -1 until read_assignment() runs.
    int my_pack = -1;

    bool read_assignment();
    cambc::bitboard assigned_target();
    void farm_camps(bool engage_shooter);

public:
    void play_turn();
};