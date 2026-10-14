// Hybrid draft: gaurab's map-adaptive scoring + our ward-first + better emblems
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

struct MapFeatures {
    int wolves = 0;
    int elks = 0;
    int gem_clusters = 0;
    int gold_clusters = 0;
    int monoliths = 0;
    int groups = 0;
    int grouped_groups = 0;
    int total_gold = 0;
    int total_gems = 0;
    int team_size = 3;
};

constexpr std::array<champion_draft, 11> kAllChampions{
    champion_draft::ryth,
    champion_draft::vesper_vale,
    champion_draft::vendetta,
    champion_draft::cassi_and_issac,
    champion_draft::stellar,
    champion_draft::prism,
    champion_draft::marcella,
    champion_draft::willow,
    champion_draft::ren,
    champion_draft::varg,
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

int score(champion_draft c, const MapFeatures& f) {
    switch (c) {
    case champion_draft::cassi_and_issac:
        return (f.grouped_groups > 0 ? 100 : 55) + f.grouped_groups + f.wolves;
    case champion_draft::varg:
        return 95 + f.grouped_groups * 4 + f.wolves;
    case champion_draft::vendetta:
        return 92 + f.wolves * 2;
    case champion_draft::ryth:
        return 88 + f.elks * 2 + f.wolves;
    case champion_draft::marcella:
        return 85 + f.wolves + (f.elks == 0 && f.team_size <= 3 ? 30 : 0);
    case champion_draft::ren:
        return 84 + f.elks + f.wolves;
    case champion_draft::vesper_vale:
        return 78 + f.gold_clusters * 2;
    case champion_draft::stellar:
        return 76 + f.elks * 3 + f.monoliths * 5;
    case champion_draft::tavi:
        return 74 + f.elks;
    case champion_draft::prism:
        return 72 + f.gem_clusters;
    case champion_draft::willow:
        return 66 + f.elks;
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
    std::string plan = "HYB601|map=";
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
    plan += " bank gems at the nearest altar; keep a body near spawn;";
    plan += " ward-first; Monster emblem; stuck detect; threat retreat.";
    return plan;
}

// Deterministic ordering of the map's camp groups, richest first. Both the
// draft and the match recompute this identically from static map data, so a
// champion can find its assigned group with no communication. Spreading the
// team across sectors is what the top of the ladder does; converging on one
// camp splits rewards and gets everyone killed together.
std::vector<std::uint8_t> assign_groups(const MapFeatures& f) {
    struct Entry {
        std::uint32_t gems;
        std::uint32_t gold;
        int tile;
        int index;
    };
    std::vector<Entry> entries;
    int i = 0;
    for (const auto& group : cambc::map::camp_groups()) {
        int first = 1 << 20;
        for (auto tile : group.spawn_tiles().positions())
            first = std::min(first, tile.y * 1000 + tile.x);
        entries.push_back({group.gem_value(), group.gold_value(), first, i++});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.gems != b.gems)
            return a.gems > b.gems;
        if (a.gold != b.gold)
            return a.gold > b.gold;
        return a.tile < b.tile;
    });
    std::vector<std::uint8_t> assign;
    if (entries.empty())
        return assign;
    for (std::size_t slot = 0; slot < static_cast<std::size_t>(f.team_size); ++slot)
        assign.push_back(static_cast<std::uint8_t>(entries[slot % entries.size()].index));
    return assign;
}

// Machine-readable tail appended after the text plan and its NUL: version,
// team size, then (champion id, camp-group index) per pick in pick order, so a
// champion can find its own sector at match time.
std::vector<std::byte> build_plan_bytes(const MapFeatures& f,
                                        const std::vector<champion_draft>& picks) {
    const std::string text = build_plan(f, picks);
    std::vector<std::byte> plan;
    plan.reserve(text.size() + 4 + picks.size() * 2);
    for (char c : text)
        plan.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    plan.push_back(std::byte{0});
    plan.push_back(std::byte{1}); // version
    plan.push_back(static_cast<std::byte>(f.team_size));
    const auto assign = assign_groups(f);
    for (std::size_t slot = 0; slot < picks.size(); ++slot) {
        plan.push_back(static_cast<std::byte>(static_cast<int>(picks[slot])));
        plan.push_back(slot < assign.size() ? static_cast<std::byte>(assign[slot])
                                            : std::byte{0});
    }
    return plan;
}

} // namespace

void play_draft() {
    const auto features = scan_map();
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);

    const auto pick = choose_next(features, chosen);

    // Pick first: a missed pick is filled randomly by the engine, which is the
    // worst possible outcome. The plan write below cannot cost us the pick.
    cambc::draft::pick(pick);

    auto picks = chosen;
    picks.push_back(pick);
    try {
        cambc::draft_memory = build_plan_bytes(features, picks);
    } catch (...) {
        // Text-only fallback: picks are already made, so the match still
        // works, just without sector assignments.
        try {
            cambc::draft_memory = std::string_view(build_plan(features, picks));
        } catch (...) {
        }
    }
}