#!/usr/bin/env python3
"""Parse Gregor telemetry logs (team-*.stdout) and summarise a match.

Usage:
    parse_telemetry.py <team-a.stdout | match.repl26a> [team-b.stdout]

Accepts either an extracted `team-*.stdout` file, a live match directory, or a
replay bundle (`.repl26a`, a zip that contains `team-a.stdout`). The engine pads
each turn's stdout with NUL bytes, so every line is stripped of NULs first.

Log format (see bots/Gregor/src/bot.cpp):
    T|r=123|my=248|opp=180|alive=3|carry=44|gold=120
    E|r=123|cid=2|DEATH|hp=0|gems=22|near=monolith
    E|r=123|cid=2|RESPAWN|hp=1400|gems=0|near=none
    E|r=123|cid=2|TRIBUTE_START|gems=22
    E|r=123|cid=2|TRIBUTE_DONE|banked=22|carried=22
    E|r=123|cid=2|CAPTURE_START|gems=44|gold=180
    E|r=123|cid=2|RECALL_START|hp=260
    E|r=123|cid=2|RECALL_CANCEL|hp=180|from=260
    E|r=123|cid=2|BUY|item=power_shard_of_health|n=2|spent=60|gold=30
    E|r=123|cid=2|ENEMY_SEEN|type=ren|pos=(24, 13)|hp=720
"""

import io
import os
import sys
import zipfile
from collections import defaultdict


def read_lines(path):
    """Yield stripped log lines from a stdout file or a replay zip."""
    text = None
    if zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as zf:
            names = [n for n in zf.namelist() if n.endswith(".stdout")]
            # Prefer the side that emitted telemetry.
            chosen = None
            for name in names:
                data = zf.read(name).replace(b"\x00", b"").decode(errors="replace")
                if any(ln.startswith(("T|", "E|")) for ln in data.splitlines()):
                    chosen = data
                    break
            text = chosen if chosen is not None else (zf.read(names[0]).replace(b"\x00", b"").decode(errors="replace") if names else "")
    else:
        with open(path, "rb") as fh:
            text = fh.read().replace(b"\x00", b"").decode(errors="replace")

    for raw in text.splitlines():
        line = raw.strip()
        if line:
            yield line


def kv(rest):
    """Parse 'a=b|c=d' into a dict."""
    out = {}
    for part in rest.split("|"):
        if "=" in part:
            k, v = part.split("=", 1)
            out[k] = v
    return out


def parse(path):
    team_samples = []          # (round, my, opp, alive, carry, gold)
    events = []                # (round, cid, kind, fields dict)
    for line in read_lines(path):
        if line.startswith("T|"):
            f = kv(line[2:])
            try:
                team_samples.append((
                    int(f["r"]), int(f["my"]), int(f["opp"]),
                    int(f["alive"]), int(f["carry"]), int(f["gold"]),
                ))
            except (KeyError, ValueError):
                pass
        elif line.startswith("E|"):
            parts = line[2:].split("|", 3)
            if len(parts) < 3:
                continue
            rfield, cidfield, kind = parts[0], parts[1], parts[2]
            rest = parts[3] if len(parts) > 3 else ""
            try:
                r = int(rfield.split("=", 1)[1])
                cid = int(cidfield.split("=", 1)[1])
            except (IndexError, ValueError):
                continue
            events.append((r, cid, kind, kv(rest)))
    return team_samples, events


def report(path):
    samples, events = parse(path)
    if not samples and not events:
        print(f"{path}: no telemetry found (did the bot emit T|/E| lines?)")
        return

    print(f"=== telemetry: {os.path.basename(path)} ===")
    if samples:
        last = samples[-1]
        first = samples[0]
        print(f"rounds observed: {first[0]}..{last[0]}  samples={len(samples)}")
        print(f"final tributed gems: my={last[1]} opp={last[2]}  "
              f"alive={last[3]} carry={last[4]} gold={last[5]}")

    # --- tributed gems over time (from TEAM samples) ---
    if samples:
        print("\n-- tributed gems over time (my/opp from T lines) --")
        step = max(1, len(samples) // 12)
        for i in range(0, len(samples), step):
            r, my, opp, alive, carry, gold = samples[i]
            print(f"  r={r:<5} my={my:<5} opp={opp:<5} alive={alive} carry={carry:<4} gold={gold}")
        if (len(samples) - 1) % step != 0:
            r, my, opp, alive, carry, gold = samples[-1]
            print(f"  r={r:<5} my={my:<5} opp={opp:<5} alive={alive} carry={carry:<4} gold={gold}")

    # --- deaths / respawns per champion, dead body-rounds, gems lost ---
    deaths = defaultdict(list)         # cid -> [round, ...]
    respawns = defaultdict(list)       # cid -> [round, ...]
    gems_lost = defaultdict(int)
    last_round = samples[-1][0] if samples else (events[-1][0] if events else 0)

    for r, cid, kind, f in events:
        if kind == "DEATH":
            deaths[cid].append(r)
            gems_lost[cid] += int(f.get("gems", 0))
        elif kind == "RESPAWN":
            respawns[cid].append(r)

    print("\n-- deaths per champion --")
    total_deaths = 0
    for cid in sorted(deaths):
        rr = deaths[cid]
        total_deaths += len(rr)
        print(f"  cid={cid}: deaths={len(rr):<3} gems_lost={gems_lost[cid]:<5} rounds={rr}")
    print(f"  TOTAL deaths: {total_deaths}  total gems lost on death: {sum(gems_lost.values())}")

    # Dead body-rounds: sum of [death, respawn) intervals, clipped to match end.
    dead_rounds = 0
    for cid, dr in deaths.items():
        rs = respawns.get(cid, [])
        for i, d in enumerate(dr):
            end = rs[i] if i < len(rs) else last_round
            dead_rounds += max(0, end - d)
    print(f"  total dead body-rounds: {dead_rounds}")

    # --- rounds between tributes (our team, from TRIBUTE_DONE) ---
    # Each TRIBUTE_DONE logs the running team total (`my`); incremental banked is
    # the delta between consecutive completions, which telescopes exactly.
    done = sorted((r, int(f.get("my", 0))) for r, cid, kind, f in events if kind == "TRIBUTE_DONE")
    cancels = sum(1 for r, cid, kind, f in events if kind == "TRIBUTE_CANCEL")
    print("\n-- tributes --")
    if not done:
        print("  none recorded")
    else:
        prev = 0
        banked = []
        for r, my in done:
            banked.append(max(0, my - prev))
            prev = my
        rounds = [r for r, _ in done]
        print(f"  count={len(done)} total_banked={sum(banked)} cancelled={cancels}")
        print(f"  rounds={rounds}")
        print(f"  banked per tribute={banked}")
        gap_list = [y - x for x, y in zip(rounds, rounds[1:])]
        if gap_list:
            print(f"  gaps between tributes: {gap_list}  "
                  f"(min={min(gap_list)} avg={sum(gap_list)/len(gap_list):.1f} max={max(gap_list)})")
    if cancels:
        print(f"  (tribute channels cancelled by damage/death: {cancels})")

    # --- gold earned vs spent ---
    capture_gold = sum(int(f.get("gold", 0)) for r, cid, kind, f in events if kind == "CAPTURE_START")
    capture_gems = sum(int(f.get("gems", 0)) for r, cid, kind, f in events if kind == "CAPTURE_START")
    spent = sum(int(f.get("spent", 0)) for r, cid, kind, f in events if kind == "BUY")
    buys = sum(1 for r, cid, kind, f in events if kind == "BUY")
    print("\n-- gold --")
    print(f"  earned from captures: {capture_gold}  (capture gems: {capture_gems})")
    print(f"  spent on purchases:   {spent}  ({buys} buy events)")

    # --- recalls ---
    rec_starts = sum(1 for r, cid, kind, f in events if kind == "RECALL_START")
    rec_cancels = sum(1 for r, cid, kind, f in events if kind == "RECALL_CANCEL")
    if rec_starts or rec_cancels:
        print("\n-- recalls --")
        print(f"  started={rec_starts} cancelled={rec_cancels}")

    # --- enemies first seen ---
    enemy_seen = [(r, cid, f.get("type"), f.get("pos"), f.get("hp"))
                  for r, cid, kind, f in events if kind == "ENEMY_SEEN"]
    if enemy_seen:
        print("\n-- first sightings of enemies --")
        for r, cid, t, pos, hp in enemy_seen:
            print(f"  r={r:<5} observer_cid={cid} type={t} pos={pos} hp={hp}")


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    for path in argv[1:]:
        report(path)
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
