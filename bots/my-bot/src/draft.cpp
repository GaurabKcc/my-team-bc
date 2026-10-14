// Hybrid draft: gaurab's map-adaptive scoring + role coverage + Cassi&Issac policy + binary tail
#include <cambc.h>
#include "draft.h"
#include <algorithm>
#include <array>
#include <cstddef>
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

constexpr bool is_tank(champion_draft c) {
    switch (c) {
    case champion_draft::ryth:
    case champion_draft::vendetta:
    case champion_draft::marcella:
    case champion_draft::ren:
    case champion_draft::varg:
    case champion_draft::cassi_and_issac: // Issac half
        return true;
    default:
        return false;
    }
}

constexpr bool is_ranged(champion_draft c) {
    switch (c) {
    case champion_draft::vesper_vale:
    case champion_draft::stellar:
    case champion_draft::prism:
    case champion_draft::willow:
    case champion_draft::tavi:
    case champion_draft::cassi_and_issac: // Cassi half
        return true;
    default:
        return false;
    }
}

void coverage_needs(int team_size, int& need_tank, int& need_ranged) {
    if (team_size >= 4) {
        need_tank = 2;
        need_ranged = 2;
    } else {
        need_tank = 1;
        need_ranged = 1;
    }
}

void count_roles(const std::vector<champion_draft>& picks, int& tanks, int& ranged) {
    tanks = 0;
    ranged = 0;
    for (auto c : picks) {
        if (is_tank(c))
            ++tanks;
        if (is_ranged(c))
            ++ranged;
    }
}

// Exact feasibility: can k remaining slots chosen from `pool` complete coverage?
bool can_complete(const std::vector<champion_draft>& chosen_after,
                  const std::vector<champion_draft>& pool,
                  int team_size) {
    int need_t, need_r;
    coverage_needs(team_size, need_t, need_r);
    int have_t, have_r;
    count_roles(chosen_after, have_t, have_r);
    int want_t = need_t - have_t;
    int want_r = need_r - have_r;
    if (want_t <= 0 && want_r <= 0)
        return true;
    int k = team_size - static_cast<int>(chosen_after.size());
    if (k <= 0)
        return false;
    if (static_cast<int>(pool.size()) < k)
        return false;
    // DFS over pool subsets of size k (pool <= 11, tiny).
    // Prune with optimistic bounds: each remaining slot can add at most 1 tank / 1 ranged.
    std::function<bool(std::size_t, int, int, int)> dfs = [&](std::size_t i, int slots, int wt, int wr) -> bool {
        if (wt <= 0 && wr <= 0)
            return true;
        if (slots == 0)
            return false;
        if (static_cast<int>(pool.size() - i) < slots)
            return false;
        // Optimistic: even taking all remaining slots as the best case.
        // One cassi pick covers both, otherwise need wt+wr <= slots (+1 if cassi still in pool).
        // Simple sound prune: wt <= slots && wr <= slots.
        if (wt > slots || wr > slots)
            return false;
        for (std::size_t j = i; j < pool.size(); ++j) {
            int left_after = static_cast<int>(pool.size() - (j + 1));
            if (left_after < slots - 1)
                break; // not enough left to fill after taking j
            int nwt = wt - (is_tank(pool[j]) ? 1 : 0);
            int nwr = wr - (is_ranged(pool[j]) ? 1 : 0);
            // Take pool[j]
            if (dfs(j + 1, slots - 1, nwt, nwr))
                return true;
            // Skip pool[j]: only if enough remain
            // (loop continues; the break condition above keeps it sound)
        }
        return false;
    };
    return dfs(0, k, want_t, want_r);
}

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

// Original map-adaptive scoring, unchanged.
int base_score(champion_draft c, const MapFeatures& f) {
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

// Cassi&Issac policy adjustments (ONLY counter-draft in this bot):
//  + early-take bonus when grouped camps exist (pair is safe to field)
//  + heavy penalty when the opponent already holds the pair (mirror-avoidance;
//    linked death is symmetric, their death is our gain - don't mirror blindly).
int score(champion_draft c, const MapFeatures& f, int picks_so_far, bool enemy_has_pair) {
    int s = base_score(c, f);
    if (c == champion_draft::cassi_and_issac) {
        if (f.grouped_groups > 0 && picks_so_far < 2)
            s += 10; // take early when grouped camps exist
        if (enemy_has_pair)
            s -= 1000; // mirror-avoidance: never outrank on score alone
    }
    return s;
}

champion_draft choose_next(const MapFeatures& f,
                           const std::vector<champion_draft>& chosen,
                           bool enemy_has_pair) {
    std::vector<champion_draft> remaining;
    for (auto c : kAllChampions) {
        if (std::find(chosen.begin(), chosen.end(), c) != chosen.end())
            continue;
        remaining.push_back(c);
    }
    if (remaining.empty())
        return champion_draft::ryth; // unreachable; engine would fill
    const int picks_so_far = static_cast<int>(chosen.size());
    // Role-coverage gate: a candidate is allowed only if remaining slots can
    // still complete 2+2 (or 1+1 for small teams).
    std::vector<champion_draft> allowed;
    for (auto c : remaining) {
        auto after = chosen;
        after.push_back(c);
        std::vector<champion_draft> pool_after;
        for (auto o : remaining) {
            if (o != c)
                pool_after.push_back(o);
        }
        if (can_complete(after, pool_after, f.team_size))
            allowed.push_back(c);
    }
    const std::vector<champion_draft>& candidates = allowed.empty() ? remaining : allowed;
    champion_draft best = candidates[0];
    int best_score = score(best, f, picks_so_far, enemy_has_pair);
    for (std::size_t i = 1; i < candidates.size(); ++i) {
        const int s = score(candidates[i], f, picks_so_far, enemy_has_pair);
        if (s > best_score) {
            best_score = s;
            best = candidates[i];
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

// ---- Versioned binary tail ----
// Layout after the text plan and its NUL:
//   magic "DPLN" (4 bytes), version u8 (=1), count u8 (= picks in order),
//   per pick: champ_id u8 (static_cast<int> of champion_draft), group u8
//             (camp-group index = pick order % num groups), pack u8 (0), flags u8 (0).
// Total tail = 6 + 4 * count bytes. Purely additive; text plan untouched.
struct TailEntry {
    std::uint8_t champ{};
    std::uint8_t group{};
    std::uint8_t pack{};
    std::uint8_t flags{};
};

std::vector<std::byte> build_memory_bytes(const std::string& text,
                                          const std::vector<champion_draft>& picks,
                                          int num_groups) {
    std::vector<std::byte> out;
    out.reserve(text.size() + 1 + 6 + 4 * picks.size());
    for (char ch : text)
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    out.push_back(std::byte{0});
    const char magic[4] = {'D', 'P', 'L', 'N'};
    for (char ch : magic)
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    out.push_back(std::byte{1}); // version
    out.push_back(static_cast<std::byte>(static_cast<unsigned char>(picks.size())));
    const int denom = num_groups > 0 ? num_groups : 1;
    for (std::size_t i = 0; i < picks.size(); ++i) {
        TailEntry e;
        e.champ = static_cast<std::uint8_t>(static_cast<int>(picks[i]));
        e.group = static_cast<std::uint8_t>(i % static_cast<std::size_t>(denom));
        e.pack = 0;
        e.flags = 0;
        out.push_back(static_cast<std::byte>(e.champ));
        out.push_back(static_cast<std::byte>(e.group));
        out.push_back(static_cast<std::byte>(e.pack));
        out.push_back(static_cast<std::byte>(e.flags));
    }
    return out;
}

} // namespace

void play_draft() {
    const auto features = scan_map();
    const auto chosen = cambc::draft::current_draft(cambc::team::ally);
    const auto enemy = cambc::draft::current_draft(cambc::team::enemy);
    const bool enemy_has_pair =
        std::find(enemy.begin(), enemy.end(), champion_draft::cassi_and_issac) != enemy.end();

    const auto pick = choose_next(features, chosen, enemy_has_pair);

    auto picks = chosen;
    picks.push_back(pick);

    // The pick must NEVER be lost to a memory-write failure: pick FIRST.
    cambc::draft::pick(pick);

    // Then submit text plan + NUL + versioned binary tail. Best-effort only.
    try {
        const std::string text = build_plan(features, picks);
        const auto bytes = build_memory_bytes(text, picks, features.groups);
        cambc::draft_memory = std::span<const std::byte>(bytes.data(), bytes.size());
    } catch (...) {
        // Pick already banked above; a tail/write failure must not cost it.
    }
}
