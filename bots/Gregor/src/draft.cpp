#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using cambc::camp_type;
using cambc::champion_draft;

// Everything we can learn about the fully-visible draft map.
struct MapFeatures {
    int wolves = 0;
    int elks = 0;
    int gem_clusters = 0;
    int gold_clusters = 0;
    int monoliths = 0;
    int groups = 0;          // number of camp groups
    int grouped_groups = 0;  // groups holding more than one camp
    int total_gold = 0;
    int total_gems = 0;
    int team_size = 3;
};

constexpr std::array<champion_draft, 11> kAllChampions{
    champion_draft::ryth,            champion_draft::vesper_vale,
    champion_draft::vendetta,        champion_draft::cassi_and_issac,
    champion_draft::stellar,         champion_draft::prism,
    champion_draft::marcella,        champion_draft::willow,
    champion_draft::ren,             champion_draft::varg,
    champion_draft::tavi,
};

MapFeatures scan_map() {
    MapFeatures f;
    for (const auto& group : cambc::map::camp_groups()) {
        ++f.groups;
        const auto n = group.camps().size();
        if (n > 1)
            ++f.grouped_groups;
        f.total_gold += static_cast<int>(group.gold_value());
        f.total_gems += static_cast<int>(group.gem_value());
        for (const auto& camp : group.camps()) {
            switch (camp.kind()) {
            case camp_type::wolf: ++f.wolves; break;
            case camp_type::elk: ++f.elks; break;
            case camp_type::gem_cluster: ++f.gem_clusters; break;
            case camp_type::gold_cluster: ++f.gold_clusters; break;
            case camp_type::monolith: ++f.monoliths; break;
            }
        }
    }
    f.team_size = static_cast<int>(cambc::draft::team_size());
    return f;
}

// The bot currently fights with basic attacks and movement only, so the best
// picks are melee bruisers that can move *and* attack, splash attackers that
// clear clustered camps, and Cassi & Issac which fields two bodies for one pick.
int score(champion_draft c, const MapFeatures& f) {
    switch (c) {
    case champion_draft::cassi_and_issac:
        // Two bodies shine when surrounding grouped camps; on maps with only
        // solo camps the extra body just feeds deaths.
        return (f.grouped_groups > 0 ? 100 : 55) + f.grouped_groups + f.wolves;
    case champion_draft::varg:
        return 95 + f.grouped_groups * 4 + f.wolves;          // splash into groups
    case champion_draft::vendetta:
        return 92 + f.wolves * 2;                             // highest attack stat
    case champion_draft::ryth:
        return 88 + f.elks * 2 + f.wolves;                    // tanky, health scaling
    case champion_draft::marcella:
        // Stun-immune bruiser; on small wolf-only maps the duo bleeds deaths,
        // so prefer a durable solo body instead.
        return 85 + f.wolves + (f.elks == 0 && f.team_size <= 3 ? 30 : 0);
    case champion_draft::ren:
        return 84 + f.elks + f.wolves;                        // retaliation
    case champion_draft::vesper_vale:
        return 78 + f.gold_clusters * 2;                      // economy, ranged
    case champion_draft::stellar:
        return 76 + f.elks * 3 + f.monoliths * 5;             // burst, but no casting yet
    case champion_draft::tavi:
        return 74 + f.elks;                                   // stun + remote shop
    case champion_draft::prism:
        return 72 + f.gem_clusters;                           // gem scaling
    case champion_draft::willow:
        return 66 + f.elks;                                   // support
    }
    return 0;
}

champion_draft choose_next(const MapFeatures& f,
                           const std::vector<champion_draft>& chosen) {
    champion_draft best = champion_draft::ryth;
    int best_score = -1;
    for (auto c : kAllChampions) {
        if (std::find(chosen.begin(), chosen.end(), c) != chosen.end())
            continue;
        const int s = score(c, f);
        if (s > best_score) {
            best_score = s;
            best = c;
        }
    }
    return best;
}

std::string build_plan(const MapFeatures& f,
                       const std::vector<champion_draft>& picks) {
    std::string plan = "cam601|map=";
    plan += (f.total_gems >= f.total_gold ? std::string("gemheavy")
                                          : std::string("goldheavy"));
    plan += "|size=" + std::to_string(f.team_size);
    plan += "|picks=";
    for (std::size_t i = 0; i < picks.size(); ++i) {
        if (i)
            plan += ',';
        plan += cambc::to_string(picks[i]);
    }
    plan += "|plan=bruise camps together; clear clustered groups first;";
    plan += " bank gems at the nearest altar; keep a body near spawn.";
    return plan;
}

} // namespace

void play_draft() {
    const auto features = scan_map();
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);

    const auto pick = choose_next(features, chosen);

    auto picks = chosen;
    picks.push_back(pick);
    cambc::draft_memory = std::string_view(build_plan(features, picks));

    cambc::draft::pick(pick);
}
