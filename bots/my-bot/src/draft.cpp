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

// 1 = tank body, 2 = ranged body, 3 = both (Cassi & Issac is one pick that
// fields a melee and a ranged champion). Packs need one of each: the tank
// steps adjacent while the ranged hits from range 3.
int role_of(champion_draft c) {
    switch (c) {
    case champion_draft::ryth:
    case champion_draft::vendetta:
    case champion_draft::marcella:
    case champion_draft::ren:
    case champion_draft::varg:
        return 1;
    case champion_draft::vesper_vale:
    case champion_draft::stellar:
    case champion_draft::prism:
    case champion_draft::willow:
    case champion_draft::tavi:
        return 2;
    case champion_draft::cassi_and_issac:
        return 3;
    }
    return 0;
}

champion_draft choose_next(const MapFeatures& f,
                           const std::vector<champion_draft>& chosen) {
    // Role coverage first: smaller teams need 1 tank + 1 ranged body, larger
    // teams 2 + 2. Cassi & Issac covers both roles at once. A candidate is
    // only allowed if the remaining slots can still complete coverage.
    const int need_each = f.team_size >= 4 ? 2 : 1;
    int tanks = 0, ranged = 0;
    for (auto c : chosen) {
        const int r = role_of(c);
        if (r & 1)
            ++tanks;
        if (r & 2)
            ++ranged;
    }
    // BISECT: coverage constraint disabled. It forced weak comps (Willow +
    // Ren) on corners and the team could not kill anything all game (0 gems,
    // 0 kills, 0 deaths). Pure score picks damage that actually clears camps.
    // role_of() is kept: pack assignment below still needs it.
    champion_draft best = champion_draft::ryth;
    int best_score = -1000000;
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

// Pack assignment, greedy over the final pick list in order: each pick joins
// the pack that most needs its role, tie-broken to the smaller pack. Pack 0
// hunts the richest group, pack 1 the second richest, so the two packs work
// different camps in parallel instead of converging.
std::vector<std::uint8_t> assign_packs(const std::vector<champion_draft>& picks) {
    std::vector<std::uint8_t> packs(picks.size(), 0);
    int pack_tank[2] = {0, 0};
    int pack_ranged[2] = {0, 0};
    int pack_size[2] = {0, 0};
    for (std::size_t i = 0; i < picks.size(); ++i) {
        const int r = role_of(picks[i]);
        int best = 0;
        int best_key = -1000000;
        for (int p = 0; p < 2; ++p) {
            int key = 0;
            if ((r & 1) && pack_tank[p] == 0)
                key += 100;
            if ((r & 2) && pack_ranged[p] == 0)
                key += 100;
            key -= pack_size[p] * 10;
            key += (p == 0) ? 1 : 0;
            if (key > best_key) {
                best_key = key;
                best = p;
            }
        }
        packs[i] = static_cast<std::uint8_t>(best);
        if (r & 1)
            ++pack_tank[best];
        if (r & 2)
            ++pack_ranged[best];
        ++pack_size[best];
    }
    return packs;
}

// Machine-readable tail appended after the text plan and its NUL: version 2,
// team size, then (champion id, camp-group index, pack) per pick in pick
// order, so a champion can find its own sector and its pack at match time.
std::vector<std::byte> build_plan_bytes(const MapFeatures& f,
                                        const std::vector<champion_draft>& picks) {
    const std::string text = build_plan(f, picks);
    std::vector<std::byte> plan;
    plan.reserve(text.size() + 4 + picks.size() * 3);
    for (char c : text)
        plan.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    plan.push_back(std::byte{0});
    plan.push_back(std::byte{2}); // version
    plan.push_back(static_cast<std::byte>(f.team_size));
    const auto assign = assign_groups(f);
    const auto packs = assign_packs(picks);
    for (std::size_t slot = 0; slot < picks.size(); ++slot) {
        const int pack = slot < packs.size() ? packs[slot] : 0;
        // BISECT: per-slot groups (pack sharing disabled) to isolate the
        // corners shutout. Pack field still written for the reader.
        const std::size_t gi = assign.empty()
            ? 0
            : slot % assign.size();
        plan.push_back(static_cast<std::byte>(static_cast<int>(picks[slot])));
        plan.push_back(assign.empty() ? std::byte{0}
                                      : static_cast<std::byte>(assign[gi]));
        plan.push_back(static_cast<std::byte>(pack));
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