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
