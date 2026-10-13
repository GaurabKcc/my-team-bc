#pragma once

#include <cambc.h>

class Bot {
    int chosen_stat = -1;
    // Spawn tiles of the camp group this champion is hunting. Held across
    // turns so it commits to one clear instead of flip-flopping between
    // near-equidistant camps, which reads as pacing back and forth.
    cambc::bitboard farm_target;
    // Altar this courier is walking to. Same commitment rationale: without it
    // a courier between two altars paces and never banks.
    cambc::bitboard bank_target;

    cambc::bitboard farm_goal(bool& committed);
    cambc::bitboard bank_goal(bool& committed);
    void submit_if_at_altar();
    void update_signal();
    bool should_help();
    void maybe_place_ward();
    bool dash_escape();
    void farm_camps(bool engage_shooter);

public:
    void play_turn();
};