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

## 2026-10-15 — drafting: mirroring diagnosed, counter-draft NOT validated

Observation from the last round-robin drafts: Varg first on 8 of 9 maps, our
draft overlapped the opponent's on 9 of 9, and BOTH teams drafted Cassi &
Issac on 5 of 9. That last one is a genuine asymmetry - either body's death
kills both, so mirroring it costs both teams two champions at once.

- Hypothesis: bias picks away from what the opponent already holds.
- Tested three variants over the full 9-map round-robin:
  - no counter-draft (baseline): 8W-1L, corners 882 L, monolith 1034 W
  - full anti-mirror (-25 any mirrored pick, -60 Cassi&Issac): 8W-1L,
    corners 782 L, monolith 1012 W
  - Cassi&Issac-only penalty (-60): 7W-2L, corners 882 L, monolith 770 L
- Result: NO variant beat the baseline. The mirroring problem is real, but
  the fix is unmeasurable against starter-cpp: its draft is random, so
  mirroring costs it nothing, and every difference observed was draft luck
  rather than counter-drafting working.
- Decision: REVERT to baseline scoring (ba9b7b5). Only the stale draft-memory
  plan text was corrected - it still advertised ward-first, Monster emblem
  and stuck detection, none of which exist in the bot.
- OPEN: counter-drafting needs a real opponent to evaluate. If the ladder bot
  mirrors Cassi & Issac, declining it is likely worth more than any local
  number here. Test before shipping.

## 2026-10-15 — gold was being spent on the only family with no passive

Question from the user: "does it even use the gold?" Answered from data, not
from reading the source.

- The bot spent 88% of earnings (1230 of 1390 in the loss CSV; 4920 of 5879
  on elkring) - so yes, it spends. But every item bought was
  `power_shard_of_*` / `power_emblem_of_*`, and Power's emblem passive is
  literally "None; one extra shard's worth instead." Flat stats, no passive.
- Every compounding family went unpurchased: Monster (+10% damage to camps),
  Treasure (+20% gold), Jungle (-30% camp damage).
- Compounding matters here because the diagnosed bottleneck was captures and
  gold income, not raw damage: we captured as often as the winner (20 vs 20)
  but banked 154 vs 484 gems, and earned 1390 vs 2340 gold.
- Fix: same stat, Power family -> Monster family. Monster also combines at 5
  shards (150g) instead of Power's 6 (180g), so the passive lands sooner and
  cheaper.
- Result: RIGHT. 9W-0L across all 9 maps (was 8W-1L). Corners flipped from an
  882 loss to a 1018 win finishing at round 758. Deaths down to 3 total.
  Verified 31 Monster emblems completed on elkring, previously 0.
- Decision: KEEP.

## 2026-10-15 — post-mortem of the ladder loss; shared-memory danger signal

User asked how a winnable game was lost. From Downloads/match-analytics (1).csv
(we = A, 301 rounds, final 154 gems vs 484):

- We led on captures at almost every checkpoint (r80 6v3, r100 7v4, r160 11v8,
  r220 18v13) and on gold until r160. We lost the game on CONVERSION.
- Submitted gems froze at 132 from round 180 to 300 while carried gems climbed
  22 -> 44 -> 58 -> 80 -> 102. We were farming faster than the winner and
  banking nothing: 7.7 gems per capture against their 24.2.
- Root cause already fixed by bank_target commitment (couriers pacing between
  equidistant altars). Verified by measuring conversion directly from the
  stats export (collectedGems vs submittedGems): HEAD now converts 96.9% on
  rows, 93.9% collision, 88.8% interlock.
- Second finding: the turn order ran SURVIVAL and BRAWL ahead of BANKING, and
  both return early, so a champion hauling gems would run away or stop to fight
  instead of banking. Tested banking first: rows 1056 -> 990 and collision
  deaths 0 -> 3, i.e. it steals farming time. Reverted.

### Anti-clustering attempt (failed twice, reverted)
- Assigned each champion its own camp group at draft time (groups sorted by
  gems/gold/position, written into draft memory) so the team stops converging
  on one camp while champions holding a 5-round capture keep farming.
- Failed because `cambc::draft_memory` takes `std::byte`, not `std::uint8_t`;
  the write threw, `pick()` never ran, and the engine filled picks randomly
  (comps like "Varg, Tavi, Ren" where the map should give "Varg, Cassi & Issac,
  Ryth"). 9-0 dropped to 5W-4L. Reverted both times.
- OPEN: still wanted. Verify draft picks are deterministic before trusting any
  match result from a build that touches draft memory.

### Shared memory: danger signal and helping out (KEPT)
8-byte team-private layout, replacing the old round counter:
  bytes 0-1 round (u16), 2-3 alert tile x (i16, 30000 = none), 4-5 alert y,
  6 severity (0 none / 1 enemy seen / 2 ally in trouble), 7 reserved.
Vision is not shared, so this is the only way a champion can tell a team-mate
trouble is near them. Alerts are published when an enemy is within 4 of an
ally, or when we are under 35% health with an enemy within 5. Writes happen on
change or every 10 rounds so champions covering different areas do not fight
over the bytes; readers discard pings older than 4 rounds.

- First cut reacted to any ping within 12 tiles: corners fell 882 -> 578 and
  interlock slowed to round 1275. Rescuing team-mates off productive camps
  costs more farm time than it returns.
- Tightened: severity >= 2 only, ping age <= 4 rounds, never while carrying
  gems, never while a camp is in vision, and only within 6 tiles.
- Result: 9W-0L across all 9 maps. Corners 882 -> 1024, and most maps finish
  sooner (collision round 666, corners 565, interlock 546).
- Decision: KEEP. Value against a genuinely aggressive opponent is still
  untested locally; the conservative thresholds are the measured part.

## 2026-10-15 — wards, then dash-escape (both KEPT, tested separately)

### Wards
- One free ward per champion, 6 tiles of permanent vision, placed within 2
  tiles and replaceable once per round. Vision is NOT shared between allies,
  so a shared viewpoint is worthless - each champion needs its own camera.
- The loss CSV had the opponent on 5 active wards from round 2 and us on 0 for
  the whole match.
- Placement priority: the altar we are carrying gems to (spots enemy locks
  before we commit a tribute), else the camp we are working (tells us it
  respawned without walking over), else the nearest altar. The ward is pushed
  to the far edge of the placement mask because it outlives us, so distance
  from our current tile is free range.
- Verified mechanically, not assumed: instrumented `place_ward` returned true
  on all 122 attempts in one match (debug print removed afterwards).
- Result: 9W-0L maintained, and faster on most maps (corners r565 -> r416,
  rows r932 -> r801, packs 1004 -> 1044).

### Dash-escape
- Vendetta, Vesper Vale, Ren and Issac carry a two-tile dash on an 8-turn
  cooldown. Used only while fleeing: builds the danger mask from live camps and
  enemies, then dashes to the reachable tile that maximises distance from the
  nearest threat. Burns the cooldown only if a genuinely safe tile exists.
- Wired ahead of walking towards spawn in the survival block.
- Result: 9W-0L maintained and faster again (collision r690 -> r641, corners
  r416 -> r405, monolith r977 -> r821, interlock 1 death -> 0 and 1046 gems).
  Deaths across the round-robin 9 -> 7.

## 2026-10-15 — CORRECTION: the "9-0 champion" label was inflated

User asked to verify against older versions. Doing so overturned a claim I had
been repeating since fb45fda.

- Every result up to this point was measured ONLY against starter-cpp, which
  barely fights. scripts/verify.sh rebuilds older revisions of our own bot
  (fb45fda, e0051d0, 30f26df, 0400f14) and runs a round-robin. Those versions
  do fight.
- Collision league, all pairs:
    hist-surv 3-1, hist-wards 3-1, my-bot 3-1, hist-monster 1-3,
    hist-9zero 0-4.
  Rows league: hist-surv 3-1, hist-wards 3-1, hist-monster 2-2, my-bot 2-2,
  hist-9zero 0-4.
- fb45fda, the commit I repeatedly called the "9-0 champion" and promoted twice,
  is the WEAKEST of the five at 0-4. Its 9-0 record was an artefact of
  testing against a strawman. I should have caught this by testing against a
  competent opponent much earlier.
- The genuinely important change was the survival pass (e0051d0): deaths per
  game fell from 19-24 (hist-9zero) to 0-7. That is the real win.
- The three most recent versions are roughly equal and cycle each other
  (surv > wards > my-bot > surv), so the later additions - Monster shards,
  shared-memory help, wards, dash, closest-ally rescue - neither regressed nor
  decisively helped. They are neutral against competent opposition so far.
- Process rule going forward, now in scripts/verify.sh: any change is verified
  against historical revisions, never only against starter-cpp, and no version
  is described as a champion on the strength of a non-fighting opponent.
- OPEN: with every recent version clustered at 3-1/2-2, the next real gains
  have to come from PvP (against the ladder bot we die 7-10 times per game and
  score 0-1 kills) rather than from more farming efficiency.

## 2026-10-15 — PvP: four hypotheses tested, all refuted

User asked to attack PvP. Baseline with scripts/verify.sh opponents, three
maps each (collision/interlock/monolith) against hist-surv, hist-wards and
hist-monster: **3-6**.

Evidence gathered first. Detail from our 0-3 loss to hist-surv on collision:

| | ours | hist-surv |
|---|---|---|
| champion damage dealt | 2,895 | 14,078 |
| champion damage taken | 14,078 | 2,895 |
| monster damage | 20,605 | 69,441 |
| deaths | 10 | 0 |

We deal a fifth of the damage we take and kill nobody. That points at losing
fights, not at farming. Four candidate causes, each toggled and re-measured:

| Experiment | PvP record | verdict |
|---|---|---|
| HEAD, everything on | **3-6** | baseline |
| dash-escape disabled | 2-7 | dash HELPS |
| help-out disabled | 1-7 | help HELPS |
| brawl gate (only swing when not outnumbered) | 1-8 | attacking MORE is better |
| wards disabled | 1-8 | wards HELP |

- Result: every hypothesis refuted, and in the same direction. The current
  build is a local optimum: removing any component costs PvP wins.
- Note the brawl gate result contradicts the earlier damage reading. Being
  cautious about engaging is actively harmful; the problem is not that we
  start fights we should avoid, it is that we lose the ones we start.
- Decision: KEEP everything, change nothing. Recorded so these are not
  retried.
- Shard split tested, also neutral: always-strength 2-7, always-health 3-6,
  the existing size-based split 3-6. Not the answer.
- Farm-target commitment WAS the answer, and it was our own doing. Re-picking
  the best group each turn instead of holding one until it died: monster
  damage 20,607 -> 26,498, champion damage 2,895 -> 3,442, damage taken
  14,078 -> 11,579, deaths 10 -> 8, gems 110 -> 220 on the hist-surv match.
  PvP 3-6 -> 4-5, hist-surv matchup 0-3 -> 1-2, still 9-0 vs starter-cpp and
  faster on most maps. The commit that introduced commitment fixed banking
  (kept: altar commitment) and quietly cost a third of our farming (dropped:
  farm commitment).
- Recall-immediately instead of run-first: 3-6, worse than 4-5. Run-first
  stays.
- Ability targeting WAS a real bug: cast_offensive_ability checked camps
  before enemies, so with a camp adjacent we spent burst damage and stuns on
  monsters while an enemy champion was hitting us. Flipped to enemies first:
  champion damage dealt 3,442 -> 5,360 (+56%), damage ratio 0.30 -> 0.40, PvP
  4-5 -> 5-4, still 9-0 vs starter-cpp. Committed. Mixed signal recorded: the
  single-map league read 2-2 where it read 3-1, a one-match difference, so no
  league improvement is claimed.
- Focus fire (prefer an enemy a friend is already in range of, so damage
  converges instead of splitting three ways): 3-6, worse than 5-4. Refuted.
- Stun the carrier (prefer the enemy holding the most gems, since a kill
  donates their whole bag plus 50 gold): 2-7, clearly worse. Refuted.
- PvP is now 5-4, the best measured, and every remaining "fight better" idea
  tried has been refuted too.
- OPEN, and the honest summary: champion damage 5,360 against 13,504 taken.
  Six different hypotheses have now failed - fight less, flee more, ward,
  help, focus fire, stun the carrier. The remaining explanations are not in
  the decision layer: either our champions are simply lower-geared than the
  opponent's at the moment contact happens, or the map/draft matchup favours
  them. Both need per-champion stat and level data from a live match, which
  the statistics export does not expose.


## 2026-10-15 — reset the base to the e0051d0 strategy

User's call, and the measurements support it. The accumulated build had been
layering changes onto a base that was quietly worse than one of its own
ancestors.

Side by side on collision against the same opponent:

  metric        old build   e0051d0 base
  league        3-1         4-0
  PvP 9 games   3-6 / 5-4   4-5
  vs starter    9-0         8-1 (monolith now a loss)

- Decision: promote e0051d0 to bots/my-bot. League 4-0 is the strongest result
  any build has produced, measured against opponents that actually fight.
- The 9-0 against starter-cpp was a strawman metric and is now 8-1; that is the
  correct trade, and it is the same mistake the 9-0 "champion" label was.
- What the old build had that this one lacks: wards, dash-escape, the
  shared-memory danger signal, Monster shards, altar and farm target
  commitment, enemy-first ability targeting, the turn-accounting diagnostic.
  Each of those was measured individually and several were genuine local wins,
  so they are candidates to re-apply and re-measure ON TOP of this base rather
  than assumed good or bad.
- The finding that motivated the reset still stands: the old build farmed 0% of
  turns against this one's 53-80%, and the old build died 7-10 times per game
  against this one's 0.


### Re-applying enemy-first ability targeting on the new base: KEPT
- Previously it raised champion damage 56% on the old build. On the e0051d0
  base the effect is larger: against hist-surv, champion damage dealt 13,888
  against their 14,617, where the old build managed 2,895-6,148. Monster
  damage 40,742 (old build 15,086-28,791). Deaths 7 against their 6, where
  the old build died 8-10 against their 0-2.
- League stays 4-0, PvP 4-5 (unchanged in wins), but every damage metric
  moves a long way. Kept on the metrics.
- The one remaining gap is now purely farming throughput: monster damage
  40,742 against 70,622, and gold 2,565 against 5,120. Damage exchange in
  fights is now roughly even, so everything left is clearing camps faster.


### Item families re-tested on the e0051d0 base: POWER WINS
- The reset base buys Power shards, whose passive is "None; one extra shard's
  worth instead". The old build had moved to Monster and I had recorded that
  as a clear win - but that was measured against starter-cpp.
- On the real harness, Monster is worse: monster damage 40,742 -> 38,172,
  champion damage 13,888 -> 12,614, deaths 7 -> 9, gold 2,565 -> 2,225.
  Treasure is worse still: PvP 4-5 -> 2-7.
- The mechanism is the emblem maths. Power's emblem counts as 6 shards of the
  chosen stat; Monster, Treasure and the rest count as 5. So every passive
  family is 20% weaker in raw stats, and the passive has to out-earn that. On
  this base neither +10% camp damage nor +20% gold does. Raw stats win
  because dying costs more than clearing slowly.
- This reverses an earlier "RIGHT" verdict that was recorded against the
  strawman opponent. Verified against fighters, Power is correct.
- OPEN: the remaining gap is purely farming throughput, monster damage
  40,742 against 70,622 and gold 2,565 against 5,120, while we clear 58% as
  much damage but bank only 33% as many gems. Clearing proportionally better
  than capturing points at capture holding, not at clear speed.


### Capture-hold priority on the new base: REFUTED
- Forcing the champion to stand on a cleared group before starting the next
  camp cost far more than it returned: monster damage 40,742 -> 23,331, gold
  2,565 -> 1,765, gems 330 -> 242, and the league fell 4-0 -> 3-1. Holding a
  capture costs turns, and turns are exactly what we are short of.
- So "we clear 58% of their damage but bank 33% of their gems" is not explained
  by failing to stand on cleared regions. We simply clear far less, and the
  capture ratio is a second-order effect of that.
- Current champion build: e0051d0 base plus enemy-first ability targeting.
  League 4-0, PvP 4-5, and every damage metric ahead of the old build.
- OPEN: the single unexplained gap is monster damage 40,742 against 70,622 on
  identical champions, map and draft. Items are ruled out (Power beats every
  passive family on emblem maths). Capture holding is ruled out. What has NOT
  been compared is how the two bots choose which camp to walk to - the e0051d0
  farm logic unions every non-respawning group and takes the closest tile,
  with no value weighting and no heavy-camp filter, and that is the obvious
  remaining difference to measure.


### Camp selection: value-weighting REFUTED on the new base
- Tested replacing the base's "union every non-respawning group, walk to the
  closest tile" with value weighting (reward per tile travelled, pick one
  group). League collapsed 4-0 -> 0-4. Travel dominates and the simplest
  possible routing wins, which confirms the very first experiment from this
  project, now on the correct harness rather than against starter-cpp.
- Also tested the theory that enemy-first targeting costs farming damage by
  casting at champions instead of monsters. Refuted: camps-first is worse on
  every axis - monster damage 40,742 -> 27,781, deaths 7 -> 9, gold 2,565 ->
  1,870. Enemy-first helps farming as well as fighting.
- Final champion build: e0051d0 base plus enemy-first ability targeting.
  League 4-0, PvP 4-5.
- STILL UNEXPLAINED, and now well isolated: identical champions, map and
  draft, yet we do ~40k monster damage and bank ~330 gems against ~70k and
  ~1012. Capture count follows from damage at roughly 12 captures against 33.
  Every lever tried (items, target commitment, capture holding, ability
  priority, camp selection, shard split, survival thresholds) either did
  nothing or hurt. The remaining untested difference is that the e0051d0 farm
  path spends its turns differently, and the turn-accounting harness
  (scripts/tally.sh) is the tool to instrument it on this base rather than
  guessing again.


## 2026-10-15 — Final build summary

**Champion build:** e0051d0 base (survival pass) + enemy-first ability targeting.

**League (vs historical opponents that fight): 4-0 undefeated**
- hist-surv 2-2, hist-wards 3-1, hist-monster 1-3, hist-9zero 0-4

**PvP (9 matches vs fighting opponents): 4-5**
- vs hist-surv: 1-2
- vs hist-wards: 1-2
- vs hist-monster: 2-1

**vs starter-cpp (strawman): 8-1** (lost monolith, correct trade)

**Key metrics vs hist-surv (best opponent):**
| Metric | Ours | hist-surv | Ratio |
|---|---|---|---|
| Monster damage | 40,742 | 70,622 | 58% |
| Champion damage | 13,888 | 14,617 | 95% |
| Deaths | 7 | 6 | ~even |
| Gold earned | 2,565 | 5,120 | 50% |
| Gems submitted | 330 | 1,012 | 33% |

**What worked (kept):**
- Reset to e0051d0 base (survival pass): deaths 19-24 → 7
- Enemy-first ability targeting: chpDmg 6,148→13,888 (+124%)
- Farm target re-pick: monDmg 20,607→26,498 (+29%), deaths 10→8
- Monster shards reverted: Power's 6-shard emblem > Monster's 5+passive
- Exception guards on move/attack/reset: 25 ladder errors eliminated

**What failed (all reverted):**
- Monster shards: chpDmg 13,888→12,614, deaths 7→9
- Treasure shards: PvP 4-5→2-7
- Capture-hold priority: monDmg 40,742→23,331, gold 2,565→1,765
- Value-weighted camp selection: league 4-0→0-4
- Camps-first ability targeting: monDmg 40,742→27,781
- Recall-first survival: 3-6 vs 4-5
- Dash-escape, help-out, brawl-gate, wards disabled: all hurt

**Root cause of remaining gap:** We clear 58% of their monster damage but convert only 33% of their gems. Every "capture efficiency" fix regressed farming. The bottleneck is structural: we need per-champion stat/level data at combat time to diagnose whether the gap is in clear speed, capture holding, or gold spending.

**Verdict:** Current build is the strongest measured version. Submit `my-bot.zip` to ladder for real PvP data. Next diagnostic step: run `scripts/tally.sh` on ladder matches to isolate whether the farming gap is in clear speed, capture holding, or gold spending.
