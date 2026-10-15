#!/bin/bash
# Per-state turn accounting for BOTH sides of a match.
#
# Both bots are instrumented (this used to patch only the opponent, which is
# how side-dependent death spirals stayed invisible). A comparison taken from
# an unmirrored pair is void: run this twice, once per order (REVERSE=1).
#
# Usage: scripts/tally.sh [hist-opponent] [map-shortname]
#        REVERSE=1 scripts/tally.sh ...   (swaps sides)
set -u
cd "$(dirname "$0")/.."
OPP="${1:-hist-surv}"
MAP_SHORT="${2:-collision}"
f=(maps/"$MAP_SHORT"*.map26a)
MAP="${f[0]}"
[ -f "$MAP" ] || { echo "no map matches '$MAP_SHORT'"; exit 1; }

case "$OPP" in
  hist-surv)    rev=e0051d0;;
  hist-wards)   rev=0400f14;;
  hist-monster) rev=30f26df;;
  hist-9zero)   rev=fb45fda;;
  hist-sector)  rev=270d418;;
  *) echo "unknown opponent $OPP"; exit 1;;
esac

# Opponent from history; ours from the working tree.
mkdir -p bots/hist-tally/src bots/my-tally/src
for f in bot.cpp bot.h draft.cpp draft.h main.cpp random.h; do
  git show "$rev:bots/my-bot/src/$f" > "bots/hist-tally/src/$f"
  cp "bots/my-bot/src/$f" "bots/my-tally/src/$f"
done

python3 - <<'PY'
patch = '''
namespace { struct T { long dead=0,recall=0,channel=0,stun=0,bank=0,flee=0,fight=0,farm=0,bankmoving=0; }; T t;
void rep(std::uint32_t r){ const long n=t.dead+t.recall+t.channel+t.stun+t.bank+t.flee+t.fight+t.farm;
 std::cout<<"TALLY r="<<r<<" tot="<<n<<" dead="<<t.dead<<" recall="<<t.recall<<" chan="<<t.channel
 <<" stun="<<t.stun<<" bank="<<t.bank<<" flee="<<t.flee<<" fight="<<t.fight<<" farm="<<t.farm
 <<" bankmoving="<<t.bankmoving<<std::endl; } }
void Bot::play_turn() { static std::uint32_t lr=0; if (cambc::round_number()>=lr+200){ lr=cambc::round_number(); rep(lr); }'''
bank_old = "    if (me.gem_count() > 0) {\n        cambc::bitboard goals;"
bank_new = "    if (me.gem_count() >= 40) {\n        ++t.bank;\n        cambc::bitboard goals;"
ret_old = ("    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()\n"
           "        || (!me.can_move() && !me.can_attack()))\n        return;")
ret_new = ("    if (me.is_recalling()) { ++t.recall; return; }\n"
           "    if (me.is_submitting_gems() || me.is_teleporting()) { ++t.channel; return; }\n"
           "    if (!me.can_move() && !me.can_attack()) { ++t.stun; return; }")
for p in ['bots/my-tally/src/bot.cpp','bots/hist-tally/src/bot.cpp']:
    s=open(p).read()
    assert 'void Bot::play_turn() {' in s, p
    s=s.replace('void Bot::play_turn() {',patch,1)
    ret_old2 = ("    if (!me.is_alive() || me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()\n"
                "        || (!me.can_move() && !me.can_attack()))\n        return;")
    if ret_old2 in s:
        # older revisions: one combined early-return does dead/recall/etc.
        s=s.replace(ret_old2,
            "    if (!me.is_alive()) { ++t.dead; return; }\n" + ret_new, 1)
    else:
        dead_anchor = "    if (!me.is_alive()) {"
        assert dead_anchor in s, p+' dead block'
        s=s.replace(dead_anchor, "    if (!me.is_alive()) { ++t.dead;", 1)
        assert ret_old in s, p+' early-return'
        s=s.replace(ret_old, ret_new, 1)
    bank_mine = "    if (me.gem_count() >= 40) {"
    bank_olds = "    if (me.gem_count() > 0) {"
    if bank_mine in s:
        s=s.replace(bank_mine, bank_mine+"\n        ++t.bank;", 1)
    elif bank_olds in s:
        s=s.replace(bank_olds, bank_olds+"\n        ++t.bank;", 1)
    s=s.replace("    farm_camps(false);", "    ++t.farm;\n    farm_camps(false);", 1)
    open(p,'w').write(s)
print('patched both sides')
PY

cambc build my-tally >/dev/null 2>&1 || { echo "build my-tally failed"; exit 1; }
cambc build hist-tally >/dev/null 2>&1 || { echo "build hist-tally failed"; exit 1; }

if [ "${REVERSE:-0}" = "1" ]; then
  timeout 500 cambc run hist-tally my-tally --map "$MAP" 2>&1 | grep -E "^  A |^  B |WIN"
else
  timeout 500 cambc run my-tally hist-tally --map "$MAP" 2>&1 | grep -E "^  A |^  B |WIN"
fi
python3 - <<'PY'
import zipfile,glob,os,re
f=sorted(glob.glob('replays/*.repl26a'),key=os.path.getmtime)[-1]
z=zipfile.ZipFile(f)
print('==',os.path.basename(f),'==')
for who,lbl in [('team-a','A'),('team-b','B')]:
    a=z.read(who+'.stdout').decode(errors='replace')
    tl=[l for l in re.split(r'[\x00]+',a) if l.startswith('TALLY')]
    print('== side',lbl,'==')
    for l in tl[-2:]: print('  ',l.strip())
PY
