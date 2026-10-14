#!/bin/bash
# Verification harness.
#
# Why this exists: the "9-0 champion" build was measured only against
# starter-cpp, which almost never fights. It actually goes 0-4 in the league
# below. Always verify against older versions of this bot, which do fight.
set -u
cd "$(dirname "$0")/.."

MAP="${1:-maps/collision-3bf50c50-7d65-4962-81e0-eec0592790e3.map26a}"

# (revision, folder) pairs to build as opponents. Older revisions of
# bots/my-bot/src, i.e. real previous strategies rather than a strawman.
HIST=(
  "fb45fda:hist-9zero"
  "e0051d0:hist-surv"
  "30f26df:hist-monster"
  "0400f14:hist-wards"
)

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

n=${#BOTS[@]}
declare -A WINS LOSSES
for b in "${BOTS[@]}"; do WINS[$b]=0; LOSSES[$b]=0; done

echo "map: $MAP (each pair plays both sides: team side decides games)"
for ((i=0;i<n;i++)); do
  for ((j=i+1;j<n;j++)); do
    for order in "$i $j" "$j $i"; do
      set -- $order
      a=${BOTS[$1]}; b=${BOTS[$2]}
      out=$(timeout 400 cambc run "$a" "$b" --map "$MAP" 2>&1)
      win=$(echo "$out" | grep -oE "teamA|teamB" | head -n1)
      sa=$(echo "$out" | grep -E "^  A " | head -n1 | awk '{print "gems="$3" kills="$4" deaths="$5}')
      sb=$(echo "$out" | grep -E "^  B " | head -n1 | awk '{print "gems="$3" kills="$4" deaths="$5}')
      if [ "$win" = "teamA" ]; then
        WINS[$a]=$(( ${WINS[$a]} + 1 )); LOSSES[$b]=$(( ${LOSSES[$b]} + 1 ))
      else
        WINS[$b]=$(( ${WINS[$b]} + 1 )); LOSSES[$a]=$(( ${LOSSES[$a]} + 1 ))
      fi
      echo "  $a vs $b -> $win | $a[$sa]  $b[$sb]"
    done
  done
done

echo "--- records (wins-losses) ---"
for b in "${BOTS[@]}"; do
  printf "  %-14s %s-%s\n" "$b" "${WINS[$b]}" "${LOSSES[$b]}"
done