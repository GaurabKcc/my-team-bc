#!/bin/bash
# Mirrored batched league harness.
#
# Why this exists:
#  1. The "9-0 champion" build was measured only against starter-cpp, which
#     almost never fights. Always verify against older revisions of this bot.
#  2. A loss means nothing until the same code wins from the other side:
#     side decides games on identical tapes. Every pair plays BOTH orders.
#  3. Single-match verdicts flip on draft luck. Batch + parallel, one
#     variable per run.
#
# Usage: scripts/verify.sh [map-shortname ...]
#   map-shortname: prefix of a file in maps/ (default: collision interlock monolith)
# Env: JOBS=8 MATCH_TIMEOUT=400 EXTRA_BOTS="bots/foo" RESULTS=path/to.csv
set -u
cd "$(dirname "$0")/.."

# (revision, folder) pairs to build as opponents. Older revisions of
# bots/my-bot/src, i.e. real previous strategies rather than a strawman.
HIST=(
  "fb45fda:hist-9zero"
  "e0051d0:hist-surv"
  "30f26df:hist-monster"
  "0400f14:hist-wards"
)
# Extra historical revisions: HIST_EXTRA="rev:name rev:name ..."
for entry in ${HIST_EXTRA:-}; do HIST+=("$entry"); done

for entry in "${HIST[@]}"; do
  rev="${entry%%:*}"
  name="${entry##*:}"
  [ -f "bots/$name/src/bot.cpp" ] && continue
  mkdir -p "bots/$name/src"
  for f in bot.cpp bot.h draft.cpp draft.h main.cpp random.h; do
    git show "$rev:bots/my-bot/src/$f" > "bots/$name/src/$f"
  done
  cambc build "$name" >/dev/null 2>&1 || echo "warning: failed to build $name"
done

BOTS=()
for entry in "${HIST[@]}"; do BOTS+=("${entry##*:}"); done
BOTS+=("my-bot")
for extra in ${EXTRA_BOTS:-}; do BOTS+=("$extra"); done
# ONLY_BOTS="a b" restricts the league (challenger vs baseline head-to-head).
if [ -n "${ONLY_BOTS:-}" ]; then
  BOTS=()
  for b in $ONLY_BOTS; do BOTS+=("$b"); done
fi

# Resolve map shortnames to files (prefix match under maps/).
map_args=("$@")
[ ${#map_args[@]} -gt 0 ] || map_args=(collision interlock monolith)
MAPS=()
for short in "${map_args[@]}"; do
  f=(maps/"$short"*.map26a)
  if [ ! -f "${f[0]}" ]; then echo "no map matches '$short'"; exit 1; fi
  MAPS+=("${f[0]}")
done

JOBS="${JOBS:-8}"
MATCH_TIMEOUT="${MATCH_TIMEOUT:-400}"
RESULTS="${RESULTS:-results.csv}"
# Per-PID namespace so multiple verify.sh instances can run in parallel
# without clobbering each other's manifest or replay outputs.
RUNID="$$"
MANIFEST=".batch-manifest-$RUNID.txt"
RUNREPLAY="replays/run-$RUNID"

# ---- build the mirrored manifest ----
rm -rf "$RUNREPLAY"; mkdir -p "$RUNREPLAY"
find replays -maxdepth 1 -name 'run-*' -mmin +1440 -exec rm -rf {} + 2>/dev/null
: > "$MANIFEST"
n=${#BOTS[@]}
m=${#MAPS[@]}
declare -A WINS LOSSES
for b in "${BOTS[@]}"; do WINS[$b]=0; LOSSES[$b]=0; done
games=0
for ((i=0;i<n;i++)); do
  for ((j=i+1;j<n;j++)); do
    for order in "$i $j" "$j $i"; do
      set -- $order
      a=${BOTS[$1]}; b=${BOTS[$2]}
      for map in "${MAPS[@]}"; do
        short=$(basename "$map"); short=${short%%-*}
        printf 'bots/%s bots/%s %s %s/vfy-%s-vs-%s-%s-%d.repl26a\n' \
          "$a" "$b" "$map" "$RUNREPLAY" "$a" "$b" "$short" "$games" >> "$MANIFEST"
        games=$((games+1))
      done
    done
  done
done

echo "league: ${BOTS[*]}"
echo "maps:   ${MAPS[*]}"
echo "games:  $games (every pair, both orders, $m map(s), $JOBS jobs)"
echo

# Replay outputs live under $RUNREPLAY (freshly cleared above); run-batch
# refuses to overwrite existing outputs, hence the per-run namespace.

# ---- run ----
out=$(timeout $(( MATCH_TIMEOUT * games / (JOBS > 0 ? JOBS : 1) + 120 )) \
      cambc run-batch --jobs "$JOBS" --timeout "$MATCH_TIMEOUT" "$MANIFEST" 2>&1)
rc=$?
echo "$out" | grep -E '^\[\+\]|failed' | sed 's/^/  /'
if [ $rc -ne 0 ]; then
  echo "run-batch exited $rc; raw output:"
  tail -20 <<<"$out" | sed 's/^/    /'
fi
echo

# ---- parse ----
# [+] TEAM A WIN (winner)  botA vs botB  mapshort  Round 0628/2000
re='^\[\+\] TEAM ([AB]) WIN \(([^)]+)\)  (.+) vs ([^ ]+)  ([^ ]+)  Round ([0-9]+)/'
ts=$(date -u +%Y-%m-%dT%H:%M:%SZ)
parsed=0
while IFS= read -r line; do
  if [[ $line =~ $re ]]; then
    side=${BASH_REMATCH[1]}; botA=${BASH_REMATCH[3]}; botB=${BASH_REMATCH[4]}
    map=${BASH_REMATCH[5]}; round=${BASH_REMATCH[6]}
    if [ "$side" = "A" ]; then wi=$botA; lo=$botB; else wi=$botB; lo=$botA; fi
    [[ -n "${WINS[$wi]+x}" && -n "${LOSSES[$lo]+x}" ]] || continue
    WINS[$wi]=$(( ${WINS[$wi]} + 1 ))
    LOSSES[$lo]=$(( ${LOSSES[$lo]} + 1 ))
    printf '%s,%s,%s,%s,%s,%s,%s\n' "$ts" "$botA" "$botB" "$map" "$wi" "$side" "$round" >> "$RESULTS"
    parsed=$((parsed+1))
  fi
done < <(grep -E '^\[\+\]' <<<"$out")

echo "--- records (wins-losses) ---"
for b in "${BOTS[@]}"; do
  printf "  %-14s %s-%s\n" "$b" "${WINS[$b]}" "${LOSSES[$b]}"
done
echo
echo "appended $parsed results to $RESULTS"
rm -f "$MANIFEST"
