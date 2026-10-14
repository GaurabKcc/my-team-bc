// Prompt 3 acceptance artifact: a unit-test-style table of camp-group scores.
//
// It mirrors the bot's camp-selection formula (reward against travel + kill time
// + capture time + risk) using the real monster design numbers, and asserts the
// required ordering: gem clusters and lone wolves rank above elks, and the
// monolith is refused.
//
// Build/run:  g++ -std=c++20 -O2 tools/camp_score_test.cpp -o /tmp/camp_score && /tmp/camp_score

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct camp_design {
    const char* name;
    float hp;
    float strength;
    int gems;
    int gold;
    int members;   // how many of this type are in the group modelled
};

// Real design values (see GameRules.md).
const camp_design kWolf{"wolf", 1350.0f, 20.0f, 22, 90, 1};
const camp_design kElk{"elk", 2400.0f, 60.0f, 36, 160, 1};
const camp_design kGemCluster{"gem_cluster", 500.0f, 0.0f, 24, 0, 1};
const camp_design kGoldCluster{"gold_cluster", 500.0f, 0.0f, 0, 35, 1};
const camp_design kMonolith{"monolith", 20000.0f, 200.0f, 200, 300, 1};

constexpr float kGemWeight = 1.0f;
constexpr float kGoldWeight = 0.08f;
constexpr float kCaptureRounds = 5.0f;
constexpr float kRiskRounds = 62.0f;  // a death costs about this many rounds

struct context {
    float team_dps;    // combined attack damage per round of the engaged team
    float team_hp;     // combined health of the engaged team
    float travel;      // rounds of travel from the champion to the group
};

struct score {
    float reward;
    float kill_time;
    float expected_taken;
    float cost;
    float value;
    bool refused;
    const char* reason;
};

score score_group(const camp_design& camp, const context& ctx) {
    const float total_hp = camp.hp * static_cast<float>(camp.members);
    const float total_str = camp.strength * static_cast<float>(camp.members);
    const float reward = kGemWeight * static_cast<float>(camp.gems * camp.members)
                       + kGoldWeight * static_cast<float>(camp.gold * camp.members);
    const float kill_time = total_hp / ctx.team_dps;
    // Members die over the fight and stop dealing damage, so about half the
    // group's full output lands.
    const float expected_taken = total_str * kill_time * 0.5f;

    score s{reward, kill_time, expected_taken, 0.0f, 0.0f, false, ""};
    if (expected_taken > ctx.team_hp * 3.0f) {
        s.refused = true;
        s.reason = "lethal";
        return s;
    }
    const float risk = (expected_taken / ctx.team_hp) * kRiskRounds;
    s.cost = ctx.travel + kill_time + kCaptureRounds + risk;
    s.value = reward / s.cost;
    return s;
}

}  // namespace

int main() {
    const context ctx{150.0f, 2000.0f, 10.0f};
    struct row { const camp_design* camp; std::string label; };
    const std::vector<row> rows{
        {&kGemCluster, "gem cluster"},
        {&kWolf, "lone wolf"},
        {&kGoldCluster, "gold cluster"},
        {&kElk, "lone elk"},
        {&kMonolith, "monolith"},
    };

    std::printf("team_dps=%.0f team_hp=%.0f travel=%.0f\n\n", ctx.team_dps, ctx.team_hp, ctx.travel);
    std::printf("%-14s %8s %10s %9s %10s %9s  %s\n",
                "group", "reward", "kill_rnds", "exp_taken", "cost", "score", "verdict");

    float gem = 0, wolf = 0, elk = 0;
    bool monolith_refused = false;
    for (const auto& r : rows) {
        const score s = score_group(*r.camp, ctx);
        if (s.refused)
            std::printf("%-14s %8.2f %10.2f %9.1f %10s %9s  refused (%s)\n",
                        r.label.c_str(), s.reward, s.kill_time, s.expected_taken, "-", "-", s.reason);
        else
            std::printf("%-14s %8.2f %10.2f %9.1f %10.2f %9.3f\n",
                        r.label.c_str(), s.reward, s.kill_time, s.expected_taken, s.cost, s.value);
        if (r.camp == &kGemCluster) gem = s.value;
        if (r.camp == &kWolf) wolf = s.value;
        if (r.camp == &kElk) elk = s.value;
        if (r.camp == &kMonolith) monolith_refused = s.refused;
    }

    std::printf("\nassertions:\n");
    int failures = 0;
    auto check = [&](bool ok, const char* msg) {
        std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", msg);
        if (!ok) ++failures;
    };
    check(gem > wolf, "gem cluster ranks above lone wolf");
    check(wolf > elk, "lone wolf ranks above lone elk");
    check(gem > elk, "gem cluster ranks above lone elk");
    check(monolith_refused, "monolith is refused");
    return failures == 0 ? 0 : 1;
}
