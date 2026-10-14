#pragma once

#include <cambc.h>
#include <cstdint>

class Bot {
    int chosen_stat = -1;
    // Sector assignment from the draft plan tail. Each champion owns a
    // different camp group so the team spreads instead of converging.
    bool assignment_read = false;
    int my_group = -1;

    bool read_assignment();
    cambc::bitboard assigned_target();
    void farm_camps(bool engage_shooter);

public:
    void play_turn();
};