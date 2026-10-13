#pragma once

// Rush-tribute: bank small gem batches fast; solo-hold captures; melee clears.
class Bot {
    bool initialised = false;
    bool is_melee = false;
    bool is_gold_farmer = false;
    int primary_buys = 0;

    void init_once();
    void buy_items();
    void maybe_ward();

public:
    void play_turn();
};
