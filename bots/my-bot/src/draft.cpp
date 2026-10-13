// Rush-tribute draft: deterministic pick order tuned for PvE tribute throughput,
// plus a small draft-memory header so champions can confirm the plan.
#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

// Pick order: bodies first (Cassi&Issac = 2 champions for 1 pick), then the
// best camp-clearing melee (Varg splash, Vendetta 70str + kill resets, Ryth),
// then gold economy (Vesper, Tavi), then stun-immune/situational picks.
constexpr std::array priority_order{
    cambc::champion_draft::cassi_and_issac,
    cambc::champion_draft::varg,
    cambc::champion_draft::vendetta,
    cambc::champion_draft::ryth,
    cambc::champion_draft::vesper_vale,
    cambc::champion_draft::tavi,
    cambc::champion_draft::marcella,
    cambc::champion_draft::ren,
    cambc::champion_draft::stellar,
    cambc::champion_draft::prism,
    cambc::champion_draft::willow,
};

void play_draft() {
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);

    // First available pick in priority order.
    cambc::champion_draft pick = priority_order.back();
    for (auto candidate : priority_order) {
        if (std::find(chosen.begin(), chosen.end(), candidate) == chosen.end()) {
            pick = candidate;
            break;
        }
    }

    // Draft memory layout: 'R','T','1', team_size, then ally picks so far
    // (as bytes) plus this pick. Read-only for champions in the match.
    std::vector<std::uint8_t> mem;
    mem.reserve(4 + chosen.size() + 1);
    mem.push_back('R');
    mem.push_back('T');
    mem.push_back('1');
    mem.push_back(static_cast<std::uint8_t>(cambc::draft::team_size()));
    for (auto c : chosen)
        mem.push_back(static_cast<std::uint8_t>(static_cast<int>(c)));
    mem.push_back(static_cast<std::uint8_t>(static_cast<int>(pick)));
    cambc::draft_memory = mem;

    cambc::draft::pick(pick);
}
