## Before acting

Read JOURNAL.md first. It holds validated findings (RIGHT), refuted ideas
(WRONG), fixed bugs (BUG), and open items (OPEN). Do not retry a WRONG
idea or re-fix a recorded BUG without a new hypothesis. Record every
significant change as a journal entry in the rule 10 format.

## Competition-Specific Strategy Rules

### 1. Optimize for tributed gems

The primary objective is permanent team gem score.

Evaluate actions by their expected contribution to tributed gems, accounting for:
- Travel time to a camp and an altar.
- Damage required to clear the complete camp group.
- Incoming monster damage and champion survival.
- Five-turn camp capture requirements.
- Tribute channel duration.
- Risk of losing carried gems.
- Competition for camps and altars.
- Gold earned and the upgrades it enables.

Do not optimize solely for monsters killed, raw gems collected, or gold held.

### 2. Treat camp clearing and capture as one task

A camp is not finished when its last monster dies.

The task lifecycle must include:
1. Approach and engage the group.
2. Clear every monster in the group.
3. Occupy a valid spawn tile.
4. Remain uncontested through capture completion.
5. Confirm that rewards were collected.
6. Select the next objective.

Do not abandon a cleared camp before securing its reward unless a higher-value emergency justifies doing so.

Avoid sending additional allies to a capture that does not benefit from additional participants. Account for reward splitting.

### 3. Make banking decisions explicit

Track carried gems, health, distance to altars, likely travel time, and relevant threats.

Tribute decisions must consider the full channel duration and interruption conditions. Do not assume a tribute completes on the turn it starts.

Recall is a separate option: it preserves carried gems but takes time and can be interrupted by damage or stun. When recalling, remain on spawn through the end of the arrival turn to receive spawn recovery.

Choose between continuing to farm, tributing, recalling, or fighting using explicit decision criteria rather than a single arbitrary gem threshold.

### 4. Respect champion-specific combat constraints

Read the champion's actual ability metadata and the official rules before implementing combat logic.

Account for:
- Melee and ranged attack allowances.
- Ability range, target categories, cooldowns, and movement restrictions.
- Attack-reset abilities.
- Damage, healing, shielding, and crowd control.
- Camp and enemy damage.
- Champion-specific passives.
- Linked deaths for Cassi and Issac.

Prefer the SDK's legality and target queries over duplicated assumptions. Check whether an action succeeded before assuming movement, a cast, or a channel began.

### 5. Use the draft as a planning phase

Drafting has a fully visible map and additional computation budget.

Evaluate champion selections against:
- Available camp rewards and their locations.
- Terrain and travel routes.
- Champion damage, survivability, and ability utility.
- Team composition and role coverage.
- The map's opportunities and risks.

Use draft memory to communicate a compact opening plan, including assignments or strategic priorities when useful. Validate the encoding and decoding on both sides of the draft/play boundary.

Do not hardcode one champion composition as universally optimal without match evidence.

### 6. Coordinate within the real memory limits

Each champion has its own bot instance. Ordinary in-process state must not be assumed to persist across turns or to be shared between champions.

Use draft memory for the initial read-only plan and the 8-byte shared buffer for compact match-time coordination.

Define a documented byte layout before using shared memory. Prefer small, robust fields such as an objective identifier, role assignment, or versioned flags when the API and map representation support them.

Never assume that one champion can inspect another champion's private vision.

### 7. Make navigation robust

Use SDK movement masks and movement helpers when they fit the task. If implementing custom pathfinding, validate it against the map's actual terrain and action rules.

Account for walls, holes, altars, occupied tiles, the enemy spawn, and special movement abilities.

A failed movement attempt can reveal a hidden obstruction without consuming the action allowance. Recover by choosing another legal destination rather than repeating the same blocked action.

Do not recompute expensive global plans unnecessarily.

### 8. Budget computation deliberately

The standard C++ budget is 100,000 fuel per turn.

Measure actual fuel usage. Keep loops bounded, reuse calculations where safe, and reserve enough computation to choose and execute a legal action.

Use the additional initial-turn budget for useful precomputation only when the result can be stored or reconstructed correctly. Do not rely on unused fuel carrying into later turns.

### 9. Build a measurable competitive baseline

Before adding sophisticated strategy:
- Produce a bot that completes a full match without crashes or fuel exhaustion.
- Verify draft picks and memory handling.
- Verify movement, combat, camp capture, tribute, recall, and shopping.
- Run repeated matches against available baseline opponents.
- Save the baseline and compare each subsequent change against it.

A sophisticated strategy that fails to bank gems reliably is worse than a simple strategy that does.

### 10. Preserve evidence and working behavior

Keep strategic parameters centralized and document their purpose.

For every significant change, record the hypothesis, baseline, test conditions, result, and decision to keep or revert.

Inspect losses for concrete causes: wasted turns, abandoned captures, unnecessary deaths, excessive tribute delays, poor champion assignments, or computation waste.

Do not declare an improvement based on a single favorable match.
