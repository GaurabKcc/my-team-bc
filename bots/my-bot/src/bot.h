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

public:
    void play_turn();
};