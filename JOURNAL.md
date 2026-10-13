# Team Journal — what works, what doesn't, and the evidence

Append-only. Never rewrite old verdicts; supersede with a new dated entry.
Every significant change records: hypothesis, baseline, test conditions,
result, decision (keep/revert), per `agents.md` rule 10.

Verdicts: RIGHT (validated) / WRONG (refuted) / OPEN (untried) / BUG (fixed fault).

Replays are gitignored; entries cite replay filenames + stats files instead.

## Environment baseline (2026-10-13)

- `cambc` 2.1.5 at `~/.local/bin/cambc` (upgraded from 2.1.4; old binary nagged
  "newer version available" because 2.1.5 published 2026-10-13 17:06 UTC).
  Verified via `/api/v1/distribution` sha256
  `6c2a0ac3f6beb1bd4ba7892212d2e09c842996d7ae12b320e8be0993c114ead5`.
- `cambc init --yes` refreshed project to `sdk_version = "2.1.5"`; `bots/my-bot`
  sources verified byte-identical before/after.
- `.gitignore` deduped (init had appended its block twice). `maps/` reduced to
  the 9 canonical pool files (`*-<uuid>.map26a`, staged as renames).
- Reference docs in repo: `GameRules.md`, `BotAPICpp.md`, `agents.md`,
  `postmortem.pdf` (Pantheon/Khaos — different game, engineering lessons only).

## Bot baselines

- `bots/my-bot`: starter clone. Random draft, random power-shard stat, banks on
  any `gems > 0`, attacks enemies in range, no abilities/wards. Frozen; do not
  modify without a journal entry.
- `bots/rush-tribute`: challenger. Deterministic draft
  (Cassi&Issac > Varg > Vendetta > Ryth > …), Monster/Treasure/Home shard
  progression, solo-hold captures, bank-on-carry couriers, retreat-before-recall,
  nearest-first routing, weakest-camp-first focus, wards, ability usage.

## A/B record: rush-tribute (A) vs my-bot (B), collision, full games

Deterministic pairing (seeded RNG, fixed spawns): same sides replay identically.

### 2026-10-13 — game 1: 0 vs 1012 (B wins, r1432)
- Hypothesis: v1 rush-tribute throughput beats starter.
- Test: `cambc run rush-tribute my-bot` + `--statistics`.
- Result: A scored 0 gems, 0 damage. Replay `team-a.stdout` showed
  `error 97: camp group is not in vision` every turn.
- Cause: BUG — unguarded `group.state()` on out-of-vision groups; main's
  catch yielded every turn.
- Decision: fix (visibility guard like the starter), rerun.

### 2026-10-13 — game 2: 216 vs 1012 (B wins, r1421)
- Hypothesis: fixed bot converts clears to score.
- Result: 216 gems; 36 deaths; collected == submitted (no bag loss, just no farm).
- Cause: BUG-class behavior — banking trips abandoned claims (capture reset),
  all bots routed to one best group, recall-under-fire fed kills.
- Decision: keep fixing (hold-before-bank, spread routing, retreat-then-recall).

### 2026-10-13 — game 3: 152 vs 1000 (B wins, r1070)
- Hypothesis: capture-hold + full-batch (10-gem) couriers win.
- Result: gold 320 → 1135 (holds work) but gems 216 → 152; 274 gems lost
  (collected 426, submitted 152) via death donations.
- Cause: WRONG — 10-gem batches kept carriers in danger; melee tanked
  Elk/Monolith pre-emblem.
- Decision: revert batching (bank on any carry), add heavy-group filter
  (skip Elk/Monolith until 5 primary buys), retreat-then-recall.

### 2026-10-13 — game 4: 438 vs 1002 (B wins, r1399)
- Hypothesis: survival fixes (retreat, immediate banking, heavy filter) close gap.
- Result: deaths 32 → 6, score 152 → 438, gold 2840. Still out-farmed ~1.7x
  (monster dmg 41k vs 78k; their Prism alone dealt 32k).
- Cause: cross-map gems-first routing — travel ate throughput.
- Decision: keep survival fixes; change routing next.

### 2026-10-13 — game 5: 1002 vs 880 (A WINS, r1533)
- Hypothesis: nearest-first routing (1 gem ≈ 2 tiles) beats gems-first.
- Result: A wins. Deaths 12 vs 39. B still earns more gold (5090 vs 3535)
  but converts worse.
- Decision: KEEP. Current `rush-tribute` is the frozen winner baseline.

### 2026-10-13 — lanes 5v5: 912 vs 1004 (B wins, r730)
- Test: `--map lanes-<uuid>`, both drafts 5 champs
  (A: +Ryth/Vesper; B: Willow/Ren/Marcella/Ryth/Prism).
- Result: close loss; A earned 0 gold (pure gem-cluster farm, 912 gems).
- Decision: OPEN — single datapoint; need multi-map round-robin before
  concluding anything about 5v5 or this map.

## Validated (RIGHT)

- Bank couriers immediately when carrying (small bags die less, donate less).
- Retreat out of camp range before recalling (recall cancels on damage).
- Solo-hold captures; never bank mid-claim; vacate after claim (respawn delay).
- Visibility-guard every `state()` call (starter pattern).
- Try/catch movement + attacks (stale snapshots cost turns, not games).
- Nearest-first routing with value tiebreak over gems-first.
- Weakest-camp-first focus fire; Monster emblem first on clearers.
- Skip Elk/Monolith until first emblem done.

## Refuted (WRONG)

- Gems-first cross-map routing (travel dominates melee throughput).
- 10-gem courier batching (carriers die, donate bags + 50g).
- Recalling in contact (channel cancels, feeds kills).
- "Melee clears 2x faster" as a blanket claim (unproven; ranged burst + uptime won damage charts — Prism 32k in game 4).

## Fixed bugs (BUG, all in rush-tribute, all verified by rerun)

1. Unguarded `group.state()` → every turn yielded (game 1).
2. Stale-snapshot `attack`/`ability.use` throws → wrapped, turn preserved.
3. Known-occupied `move_absolute` throws → `move_towards_closest` returns false.
4. Deleted-by-edit pre-move attack loop → restored (burn in-range before walking).
5. `claim = &group` dangling pointer → restructured to act inside the loop.

## Open (OPEN, with rule references)

- Explicit banking scorer (agents.md#3): value(bank) vs value(one more camp).
- Fuel audit: log `fuel_remaining()`, bound loops (agents.md#8).
- Memory contract: documented byte layouts, draft encode/decode validation,
  test `Bot` member survival across death (agents.md#6).
- Map-aware draft with camp/altar assignments in draft memory (agents.md#5).
- Kit specialization: Cassi/Issac linked-death handling, Prism carry synergy (agents.md#4).
- PDA resume stack (contained courier experiment first).
- Threat-aware flight A\*; tank funnel via solo-capture gold concentration.
- Voronoi task partition; scored states with ceilings; stuck detection + TTL caches (postmortem adoptions).
- Multi-map round-robin; mirrored-side A/B; altar denial play.

## Compliance snapshot (agents.md audit, 2026-10-13)

Rules 1, 2, 7, 9 compliant. Rule 3 violated (arbitrary bank threshold).
Rule 4 partial (no linked-death/kit play). Rule 5 partial (fixed draft order).
Rule 6 partial (no byte layout; persistence unverified). Rule 8 unmeasured.
Rule 10 fixed by this file.

## 2026-10-14 — port rush-tribute winner into my-bot, dispose rush-tribute
- Hypothesis: copied sources behave identically under the `my-bot` name.
- Baseline: rush-tribute game 5 (1002 vs 880 W, r1533, collision).
- Test: `diff -r bots/my-bot/src bots/rush-tribute/src` (identical), then
  `cambc build my-bot` + `cambc run my-bot starter-cpp --statistics
  /tmp/opencode/port-validate.json`.
- Result: RIGHT — my-bot wins 1002 vs 880 in round 1533, same draft
  (Cassi&Issac/Varg/Vendetta vs Cassi&Issac/Tavi/Prism), zero crashes.
  Deterministic tape reproduced exactly, port is behavior-identical.
- Decision: KEEP. `bots/rush-tribute/` deleted; all future bot work happens
  on `my-bot`. `starter-cpp` untouched as pristine upstream baseline.
- Commit: `.gitignore` (+entries, `*.pdf`), `cambc.toml` 2.1.5, `agents.md`
  (+read-journal preamble), `JOURNAL.md`, 9 pool-map renames, ported
  `bots/my-bot/src/*`, `BotAPICpp.md`, `GameRules.md`. `postmortem.pdf`
  excluded (now covered by `*.pdf` ignore).

## 2026-10-14 — pack-hunt experiment (50/50 split + HP-swap at 40%)

- Hypothesis: split packs onto two groups + healthy steps in for hurt allies
  (<40%) while low ally recalls; weakest-enemy-first kills.
- Baseline: winner 1002 vs 880 (r1533, collision).
- Tests (all `cambc run my-bot starter-cpp`, collision, full games):
  - Full batch (split + step-in + flat 40% + weakest-enemy): 680 vs 1010 L.
  - Split fallback (>=60% rule): identical 680 vs 1010 (split always active).
  - Split disabled (step-in + 40% + weakest-enemy kept): 668 vs 1018 L —
    split was not the sole cause.
  - Step-in ONLY on winner baseline: 854 vs 1004 L (r1581).
- Result: WRONG as implemented. Step-in alone cost ~150 gems: healthy
  farmers walk off their camps toward hurt allies (travel waste, shared
  danger). Deaths stayed low (7-10), damage held (~42-47k), but conversion
  dropped — banking/conversion suffered, not survival.
- Lesson (agents.md#10): batched 4 variables in one A/B; attribution took
  3 extra full games. One variable per A/B from now on.
- Decision: REVERT all — `my-bot` restored byte-identical to 2452dae winner.
- OPEN: constrained step-in (mate within <=3 AND already sharing the camp,
  i.e. zero travel) remains untried.

## 2026-10-14 — round-robin: my-bot vs starter-cpp, all 9 pool maps

- Baseline: 2452dae winner. Full games, collision default otherwise.
- Result: 5W-4L.
  - W: collision 1002-880 (r1533), corners 1014-526 (r824),
    expanse 1022-880 (r865), packs 1006-620 (r745), rows 1012-792 (r871).
  - L: elkring 0-1008 SHUTOUT, interlock 478-1008, lanes 912-1004,
    monolith 880-1004.
- Team sizes are map-defined (3-5 picks); draft fills every slot offered
  (3: collision/expanse/packs; 4: interlock/rows; 5: corners/elkring/lanes/
  monolith). No way to force 5 slots — size comes from the map.
- Zero crashes in all 9 games.

## 2026-10-14 — elkring deadlock fix (0 dmg in 1008 rounds)

- Hypothesis: heavy-group filter (skip Elk/Monolith pre-emblem) deadlocks
  maps with only heavy groups — 0 gold means 0 emblems forever.
- Evidence: elkring A had 0 gems/0 dmg/0 deaths/0 errors; B (no filter)
  farmed the same camps to 5850 gold.
- Fix: two-pass farm targeting — skip heavy on pass 0, engage best-heavy
  on pass 1 if pass 0 found nothing. Pass-0 behavior unchanged, so other
  maps' deterministic tapes are unaffected.
- Test: `--map elkring-<uuid>` rerun.
- Result: RIGHT — 1008 vs 378 W in round 671 (was 0 vs 1008 L).
  Round-robin stands at 6W-3L with the fix.
- OPEN: interlock (lost PvP 6-16 kills), lanes 5v5, monolith — not yet
  diagnosed.

## 2026-10-14 — gaurab-branch analysis (9-0 bot)

- Checked out origin/gaurab-branch (eee1932 "Make my-bot map-adaptive and beat starter on all 9 maps").
- Verified 9-0 vs starter-cpp on all 9 maps (confirmed).
- Key design: map-adaptive draft scoring (scans camps, picks Varg/Vendetta/Cassi&Issac based on grouped camps/wolves/elks), claims-based Voronoi partition (unused in practice), simple ability usage (attack-reset + offensive/defensive), two-pass heavy filter NOT present (engages all camps).
- Key insight: extreme simplicity wins. No wards, no stuck detection, no HP-swap, no threat retreat, Power shards (no passive). The 9-0 bot is ~300 lines vs our 400+.

## 2026-10-14 — hybrid bot iterations

### Full hybrid (gaurab draft + our additions)
- Added: ward-first, Monster→Home emblems, stuck detection, threat retreat, HP-swap, claims filter, two-pass heavy filter.
- Result: 6-3 vs starter (same as my-bot), lost head-to-head vs gaurab-test 240-1000.
- Diagnosis: claims filter prevented valid targets; HP-swap wasted travel; threat retreat wasted turns; ward-first took farm turns.

### Minimal hybrid (gaurab core + Monster→Home emblems only)
- Stripped ALL additions except: Monster→Home emblem progression (Power shards → Home after 5).
- Kept gaurab's exact logic: map-adaptive draft, simple abilities, 0.35f recall, 0.6f defensive, attack-reset combo, engage-shooter farm, no claims, no wards, no stuck, no HP-swap, no threat retreat.
- Result: 9-0 vs starter (all 9 maps), beats gaurab-test 1012-798 head-to-head, crushes my-bot 1012-44.
- Faster wins on 7/9 maps vs gaurab-test (collision 760 vs 799, monolith 760 vs 955, packs 570 vs 635).
- Decision: PROMOTE to my-bot. This is the new champion.

## 2026-10-14 — my-bot promoted to minimal hybrid (9-0 champion)

- `bots/my-bot/src/*` replaced with minimal hybrid sources.
- Verified: collision 1012-286 W (r760), 9-0 round-robin confirmed.
- Beats gaurab-test 1012-798 head-to-head.
- Crushes old my-bot 1012-44.
- Zero crashes all games.

## 2026-10-14 — claims/Voronoi experiment (failed)

- Hypothesis: claims/Voronoi partition prevents ally clustering on same camps.
- Test: added `claims()` Voronoi filter to `farm_camps` goals.
- Results: 6-3 vs starter (was 9-0), MORE deaths on interlock/rows, slightly better on lanes.
- Mechanism: Voronoi too aggressive — starved bots of targets, prevented allies from helping each other, broke the natural distance-weighted spread.
- Lesson: distance-weighted scoring already spreads allies naturally; Voronoi is overkill and breaks cooperation.
- Decision: REVERTED. 9-0 version restored.

## 2026-10-15 — survival pass: run-then-recall + cast-after-move fix

Context: user reported the team "gets pummeled" and champions keep fighting
at low HP. Platform CSV (Downloads/match-analytics.csv, we were Team A)
showed the real shape of the loss: 372 gems, 17 captures, 0 kills,
160 gold, 3/6 alive vs 1016/46/5/1060/6/6. Deaths donate the whole gem bag
plus 50 gold, so survival converts directly into score.

- Hypothesis 1: champions call `recall()` while a camp/enemy can still reach
  them. Recall is cancelled by damage, so the channel restarts or they die.
  Fix: run clear of damage FIRST, recall only when nothing can hit us.
  Bail threshold: 30% HP, or 50% while carrying gems (dying costs the bag).
- Hypothesis 2: `farm_camps` moved, then called `cast_offensive_ability()`.
  "Excludes walking" spells throw `error 126` after a move; main.cpp caught
  it and ended the turn, wasting it. Measured 23 occurrences on collision.
  Fix: after a move, re-check camps for a basic attack but never cast.
- Test conditions: `cambc run my-bot starter-cpp`, all 9 pool maps, full games.

- Result: RIGHT. Record 8W-1L. Deaths collapsed: collision 16 -> 2, and
  10 total deaths across all 9 games. Error-126 occurrences 23 -> 0.
  Wins got faster (collision 1012 by round 675 vs 760 before).
- Threshold tune: relaxing the non-carrier bail 0.30 -> 0.35 made monolith
  WORSE (924 -> 836). Deaths donate gems, so caution pays. Kept 0.30.
- Regression: monolith 924-1032, the one loss (champion won it 1018-564).
  We take 12 kills / 0 deaths there but farm gems slower than a bot that
  dies 48 times. OPEN: map-specific aggressiveness for monolith.
- Process lesson (rules 8 and 10, now enforced): measure before changing.
  Three of my intermediate builds scored 0 gems/damage and I mis-attributed
  the cause twice ("wards block turns" was wrong - the call was commented out).
  Fuel was instrumented and ruled out (65-85k of 100k spent, never exhausted).
  The 223 "faults" in one build came from an over-engineered flee path that
  redrew chebyshev bitboards per camp per turn; replaced with a cheap
  distance check. Kept only the two changes that measured clean.
- Decision: KEEP both fixes. Skipped the redundant brawl-gating and defensive
  -ability-restructure experiments (the survival block returns before brawl).

## 2026-10-15 — target commitment (anti-oscillation + banking fix)

Triggers: (a) user reported champions pacing back and forth with no target;
(b) platform CSV (Downloads/match-analytics (1).csv, we = A) showed the loss
shape: 154 gems vs 484 on IDENTICAL captures (20 vs 20) - 7.7 gems/capture
vs 24.2. Carried gems never cleared: 22 at round 25, 80 by round 225, 102 at
round 300. Submitted gems froze at 132 for 125 rounds. Opponent had 5 wards,
we had 0.

- Root cause: both farming and banking built a goal set as the UNION of every
  group / altar each turn, then stepped toward whichever neighbour strictly
  reduced distance. A champion standing between two near-equidistant camps
  (or altars) flips sides every turn as occupancy changes. Couriers never
  reached an altar, so bags sat unbanked.
- Fix: commit to one target and hold it across turns. `farm_target` (spawn
  tiles of the chosen camp group) and `bank_target` (chosen altar); both
  re-picked only when the target dies, respawns, or gets locked.
- Regression found and fixed: committing to the NEAREST group walked
  champions into Elk/Monolith groups and held them there - rows fell to
  66 gems / 40 deaths, monolith to 498. Committing concentrates where
  union-goals had spread. Fix: two-pass selection, light groups only on
  pass 0, heavy only when no light group remains (same shape as the earlier
  elkring deadlock fix). rows 66 -> 1056, monolith 498 -> 1034.
- Result: RIGHT. 8W-1L across all 9 maps, 5 total deaths (10 before).
  Gem scores up across the board (interlock gold 1390 -> 2010-3620).
  Corners is now the only loss (882-1038), previously a 1006-150 win.
- Decision: KEEP. Corners regression logged OPEN.
