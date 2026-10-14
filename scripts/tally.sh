#!/bin/bash
# Per-state turn accounting for both sides of a match.
# This is what showed we farm 0% of turns while hist-surv farms 53-80%.
set -u
cd "$(dirname "$0")/.."
OPP="${1:-hist-surv}"
MAP="${2:-maps/collision-3bf50c50-7d65-4962-81e0-eec0592790e3.map26a}"

# Build the opponent from history with the same tally instrumentation.
mkdir -p bots/hist-tally/src
for f in bot.cpp bot.h draft.cpp draft.h main.cpp random.h; do
  case "$OPP" in
    hist-surv)   git show e0051d0:bots/my-bot/src/$f > bots/hist-tally/src/$f;;
    hist-wards)  git show 0400f14:bots/my-bot/src/$f > bots/hist-tally/src/$f;;
    hist-monster)git show 30f26df:bots/my-bot/src/$f > bots/hist-tally/src/$f;;
    hist-9zero)  git show fb45fda:bots/my-bot/src/$f > bots/hist-tally/src/$f;;
    *) echo "unknown opponent $OPP"; exit 1;;
  esac
done
python3 - <<'PY'
import re
p='bots/hist-tally/src/bot.cpp'
s=open(p).read()
s=s.replace('void Bot::play_turn() {','''namespace { struct T { long dead=0,recall=0,channel=0,stun=0,bank=0,flee=0,fight=0,farm=0,bankmoving=0; }; T t;
void rep(std::uint32_t r){ const long n=t.dead+t.recall+t.channel+t.stun+t.bank+t.flee+t.fight+t.farm;
 std::cout<<"TALLY r="<<r<<" tot="<<n<<" dead="<<t.dead<<" recall="<<t.recall<<" chan="<<t.channel
 <<" stun="<<t.stun<<" bank="<<t.bank<<" flee="<<t.flee<<" fight="<<t.fight<<" farm="<<t.farm
 <<" bankmoving="<<t.bankmoving<<std::endl; } }
void Bot::play_turn() { static std::uint32_t lr=0; if (cambc::round_number()>=lr+200){ lr=cambc::round_number(); rep(lr); }''',1)
s=s.replace('''    if (!me.is_alive() || me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()
        || (!me.can_move() && !me.can_attack()))
        return;''','''    if (!me.is_alive()) { ++t.dead; return; }
    if (me.is_recalling()) { ++t.recall; return; }
    if (me.is_submitting_gems() || me.is_teleporting()) { ++t.channel; return; }
    if (!me.can_move() && !me.can_attack()) { ++t.stun; return; }''',1)
s=s.replace('''    if (me.gem_count() > 0) {
        cambc::bitboard goals;''','''    if (me.gem_count() > 0) {
        ++t.bank;
        cambc::bitboard goals;''',1)
s=s.replace('    farm_camps(false);','    ++t.farm;\n    farm_camps(false);',1)
open(p,'w').write(s)
PY
cambc build hist-tally >/dev/null 2>&1 || { echo "build failed"; exit 1; }
timeout 500 cambc run my-bot hist-tally --map "$MAP" 2>&1 | grep -E "^  A |^  B "
python3 - <<'PY'
import zipfile,glob,os,re
f=sorted(glob.glob('replays/*.repl26a'),key=os.path.getmtime)[-1]
z=zipfile.ZipFile(f)
for who,lbl in [('team-a','OURS'),('team-b','OPPONENT')]:
    a=z.read(who+'.stdout').decode(errors='replace')
    tl=[l for l in re.split(r'[\x00]+',a) if l.startswith('TALLY')]
    print('==',lbl,'==')
    for l in tl[-2:]: print('  ',l.strip())
PY
