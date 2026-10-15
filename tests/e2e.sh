#!/bin/sh
# End to end on Linux: tscli (built with OpenSSL) against a local HTTPS
# stand-in for YouTube (fake_youtube.py) answering with the recorded
# fixtures. Checks the lists, the chosen streams and every request.
#   tests/e2e.sh TUBECLI
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CLI=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
W=$(mktemp -d)
PRX=
trap 'kill $SRV $PRX 2>/dev/null || true; rm -rf "$W"' EXIT
FIX=$HERE/fixtures
[ -s "$FIX/next.json" ] || { echo "no fixtures: run tests/fetch-fixtures.sh" >&2; exit 1; }
python3 -I -c "
from PIL import Image
Image.new('RGB', (320, 180), (200, 30, 30)).save('$FIX/thumb.jpg', quality=80)"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$W/key.pem" -out "$W/cert.pem" \
  -days 2 -subj /CN=www.youtube.com >/dev/null 2>&1
# the solver scripts: the bundled copy, and versions the fake GitHub offers
EJS=$(cd "$HERE/../ejs/0.8.0" && pwd)
mkdir -p "$W/bundle" "$W/state" "$W/work" "$W/ejs/0.8.1-test" "$W/ejs/0.8.2-bad" "$W/ejs/0.8.3-broken"
cp "$EJS/yt.solver.lib.min.js" "$W/bundle/lib"; cp "$EJS/yt.solver.core.min.js" "$W/bundle/core"; echo 0.8.0 > "$W/bundle/Version"
for v in 0.8.1-test 0.8.2-bad; do
  { cat "$EJS/yt.solver.lib.min.js"; printf '\n// %s\n' $v; } > "$W/ejs/$v/yt.solver.lib.min.js"
  { cat "$EJS/yt.solver.core.min.js"; printf '\n// %s\n' $v; } > "$W/ejs/$v/yt.solver.core.min.js"
done
cp "$EJS/yt.solver.lib.min.js" "$W/ejs/0.8.3-broken/"
echo 'var jsc = function () { throw new Error("broken on purpose"); };' > "$W/ejs/0.8.3-broken/yt.solver.core.min.js"
manifest() {   # manifest VERSION [BADHASH]
  python3 -I -c "
import hashlib,json,sys
v,bad,d=sys.argv[1],sys.argv[2]=='bad',sys.argv[3]
h=lambda n: hashlib.sha3_512(open(d+'/'+v+'/'+n,'rb').read()).hexdigest()
f={n:h(n) for n in ('yt.solver.lib.min.js','yt.solver.core.min.js')}
if bad: f['yt.solver.core.min.js']='0'*128
open(d+'/manifest.json','w').write(json.dumps({'version':v,'files':f}))" "$1" "${2:-}" "$W/ejs"
}
CLI_TV="$CLI -ejs $W/bundle -state $W/state -work $W/work"
PORT=$((20000 + $$ % 20000))
python3 -I "$HERE/fake_youtube.py" $PORT "$W/cert.pem" "$W/key.pem" "$FIX" "$W/log" "$W/ejs" > "$W/srv.out" 2>&1 &
SRV=$!
for i in 1 2 3 4 5 6 7 8 9 10; do grep -q ready "$W/srv.out" 2>/dev/null && break; sleep 0.3; done
export TUBE_CONNECT=127.0.0.1:$PORT TUBE_INSECURE=1
fail=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi; }

"$CLI" -more 1 search test > "$W/search" 2> "$W/err" || { cat "$W/err"; fail=1; }
check "search: 34 results over two parts" "[ \$(grep -c '^[A-Za-z0-9_-]\{11\} ' '$W/search') -eq 33 ] && grep -q '^\[channel\] UCZRcA_nO3Kt-SOml2mY8D9g' '$W/search' && grep -q '^-- more' '$W/search'"
"$CLI" related 7PIMiDcwNvc > "$W/related" 2> "$W/err" || { cat "$W/err"; fail=1; }
check "related: 20 videos and the title" "grep -q '^== Marzia & Felix - Wedding 19.08.2019' '$W/related' && [ \$(grep -c '^[A-Za-z0-9_-]\{11\} ' '$W/related') -eq 20 ]"
"$CLI" -more 1 channel UCTwECeGqMZee77BjdoYtI2Q > "$W/channel" 2> "$W/err" || { cat "$W/err"; fail=1; }
check "channel: 60 videos" "grep -q '^== Creative Commons' '$W/channel' && [ \$(grep -c '^[A-Za-z0-9_-]\{11\} ' '$W/channel') -eq 60 ]"
"$CLI" play 7PIMiDcwNvc 720 "$W/play.json" 2> "$W/err" || { cat "$W/err"; fail=1; }
check "play: 720p H.264 + AAC as HLS for Reel" "grep -q '720p avc1.4D401F + AAC (HLS)' '$W/err' && python3 -I -c \"
import json,sys
d=json.load(open('$W/play.json'))
f=d['requested_formats']
assert '/itag/232/' in f[0]['url'] and f[0]['acodec']=='none' and f[0]['protocol']=='m3u8_native'
assert 'itag%3D140/playlist' in f[1]['url'] and f[1]['vcodec']=='none'
assert d['webpage_url'].endswith('7PIMiDcwNvc')
\""
printf 'stream_format = dash\n' > "$W/dash.cfg"
"$CLI" -config "$W/dash.cfg" play 7PIMiDcwNvc 720 "$W/dash.json" 2> "$W/err" || { cat "$W/err"; fail=1; }
check "play with stream_format = dash: the DASH pair" "grep -q '720p avc1' '$W/err' && python3 -I -c \"
import json,sys
d=json.load(open('$W/dash.json'))
f=d['requested_formats']
assert 'itag=136' in f[0]['url'] and f[0]['acodec']=='none' and 'protocol' not in f[0]
assert 'itag=140' in f[1]['url'] and f[1]['vcodec']=='none'
\""
check "every request as YouTube expects" "python3 -I -c \"
import json
rows=[json.loads(l) for l in open('$W/log')]
bad=[r for r in rows if r.get('problems')]
assert not bad, bad
eps=[r.get('endpoint') for r in rows]
assert eps.count('visitor_id')==5, eps
assert eps.count('player')==2 and eps.count('search')==2 and eps.count('browse')==2 and eps.count('next')==1, eps
\""
"$CLI" play badvideoid 720 > /dev/null 2> "$W/err" && fail=1
check "an unplayable answer is reported" "grep -q 'tscli:' '$W/err'"
# ---- the TV fallback: QuickJS answering the challenges ----
count() { python3 -I -c "
import json,sys
rows=[json.loads(l) for l in open('$W/log')]
print(sum(1 for r in rows if (r.get('endpoint')==sys.argv[1] or r.get('path','').startswith(sys.argv[1]))))" "$1"; }
tv_ok() {   # tv_ok FILE: Reel's file has the answered addresses
  python3 -I -c "
import json,sys
d=json.load(open(sys.argv[1])); f=d['requested_formats']
v,a=f[0]['url'],f[1]['url']
assert 'itag=136' in v and 'n=RL7twTMlYAhbWlqMF' in v and 'IlLiA21ny7gqA2m4p37' not in v, v[-300:]
assert v.endswith('&sig=zzwg%3Dwgw' + v.split('&sig=zzwg%3Dwgw')[1]) and v.endswith('jiEJA'), v[-200:]
assert 'itag=140' in a and 'n=OtjoLvmzT_c2ob_ei' in a, a[-300:]" "$1"; }
s=$(date +%s)
$CLI_TV -client tv play 7PIMiDcwNvc 720 "$W/tv.json" 2> "$W/err" || { cat "$W/err"; fail=1; }
t1=$(( $(date +%s) - s ))
check "TV client: the challenges answered by QuickJS (${t1}s)" "tv_ok '$W/tv.json'"
check "TV client: the player's ID, its code, then the TV answer" "[ \$(count /iframe_api) = 1 ] && [ \$(count /s/player/854a788e/) = 1 ] && [ \$(count player-tv) = 1 ]"
check "the prepared player and timestamp are kept" "ls '$W/work/Players/854a788e-0-8-0' '$W/work/Players/854a788e-sts' >/dev/null"
s=$(date +%s)
$CLI_TV play BotCheck001 720 "$W/auto.json" 2> "$W/err" || { cat "$W/err"; fail=1; }
t2=$(( $(date +%s) - s ))
check "auto: a 'not a bot' refusal falls back to the TV client (${t2}s, prepared player reused)" "tv_ok '$W/auto.json' && grep -q 'TV app' '$W/err' && [ \$(count /s/player/854a788e/) = 1 ]"
check "no job or answer files left behind" "! ls '$W/work' | grep -q 'TubeSearchJob\|TubeSearchAns'"

# ---- keeping the solver up to date ----
manifest 0.8.1-test
$CLI_TV update 2> "$W/err"
check "update: a newer version from the manifest, through GitHub's redirect, hash checked" "grep -q 'solver scripts 0.8.1-test taken' '$W/err' && [ \"\$(cat '$W/state/Version')\" = 0.8.1-test ] && cmp -s '$W/state/core' '$W/ejs/0.8.1-test/yt.solver.core.min.js'"
$CLI_TV update 2> "$W/err"
check "update again: already up to date" "grep -q 'up to date' '$W/err'"
$CLI_TV -client tv play 7PIMiDcwNvc 720 "$W/tv2.json" 2> "$W/err" || { cat "$W/err"; fail=1; }
check "the new scripts solve (a new prepared player for them)" "tv_ok '$W/tv2.json' && ls '$W/work/Players/854a788e-0-8-1-test' >/dev/null && ! ls '$W/work/Players/854a788e-0-8-0' 2>/dev/null"
# a newer copy in TubeSearch itself beats an older download
echo 0.9.0 > "$W/bundle/Version"
$CLI_TV -client tv play 7PIMiDcwNvc 720 "$W/newer.json" 2> "$W/err" || { cat "$W/err"; fail=1; }
check "a bundled copy newer than the download is used" "tv_ok '$W/newer.json' && ls '$W/work/Players/854a788e-0-9-0' >/dev/null"
echo 0.8.0 > "$W/bundle/Version"
# a damaged prepared player: thrown away and prepared again from the code, scripts not blamed
$CLI_TV -client tv play 7PIMiDcwNvc 720 "$W/x.json" 2> /dev/null
echo 'this is not a prepared player' > "$W/work/Players/854a788e-0-8-1-test"
$CLI_TV -client tv play 7PIMiDcwNvc 720 "$W/damaged.json" 2> "$W/err" || { cat "$W/err"; fail=1; }
check "a damaged prepared player is replaced, and the scripts aren't blamed" "tv_ok '$W/damaged.json' && ! grep -q '^bad' '$W/state/State' && [ \$(wc -c < '$W/work/Players/854a788e-0-8-1-test') -gt 100000 ]"
manifest 0.8.2-bad bad
$CLI_TV update 2> "$W/err"
check "a hash that doesn't match: refused, the old ones kept" "grep -q \"didn't match\" '$W/err' && [ \"\$(cat '$W/state/Version')\" = 0.8.1-test ]"
manifest 0.8.3-broken
$CLI_TV update 2> "$W/err"
$CLI_TV -client tv play 7PIMiDcwNvc 720 "$W/broken.json" 2> "$W/err" || { cat "$W/err"; fail=1; }
check "scripts that don't work: marked bad, and the same play carries on with the bundled copy" "tv_ok '$W/broken.json' && grep -q 'bad 0.8.3-broken' '$W/state/State'"
$CLI_TV -client tv play 7PIMiDcwNvc 720 "$W/after.json" 2> "$W/err" || { cat "$W/err"; fail=1; }
check "and afterwards too" "tv_ok '$W/after.json'"
$CLI_TV update 2> "$W/err"
check "a bad version isn't taken again" "grep -q 'up to date' '$W/err'"
check "every TV request as YouTube expects" "python3 -I -c \"
import json
bad=[r for r in (json.loads(l) for l in open('$W/log')) if r.get('problems')]
assert not bad, bad\""

# ---- the upload-date filter ----
"$CLI" -date week search test > "$W/week" 2> "$W/err" || { cat "$W/err"; fail=1; }
check "-date week: YouTube's This week filter sent" "python3 -I -c \"
import json
r=[x for x in (json.loads(l) for l in open('$W/log')) if x.get('endpoint')=='search'][-1]
assert r['params']=='EgIIAw==' and r['query']=='test', r\""

# ---- through a web proxy ----
PPORT=$((PORT + 1))
python3 -I "$HERE/fake_proxy.py" $PPORT $PORT chris:se@cret "$W/plog" > "$W/prx.out" 2>&1 &
PRX=$!
for i in 1 2 3 4 5 6 7 8 9 10; do grep -q ready "$W/prx.out" 2>/dev/null && break; sleep 0.3; done
pcount() { python3 -I -c "
import json,sys
print(sum(1 for l in open('$W/plog') for r in [json.loads(l)] if r['target']==sys.argv[1] and r['auth_ok']))" "$1"; }
"$CLI" -proxy "http://chris:se%40cret@127.0.0.1:$PPORT/" search test > "$W/psearch" 2> "$W/err" || { cat "$W/err"; fail=1; }
check "-proxy: the search goes through the proxy, signed in" "grep -q 'through the proxy 127.0.0.1:$PPORT' '$W/err' && [ \$(grep -c '^[A-Za-z0-9_-]\{11\} ' '$W/psearch') -eq 23 ] && [ \$(pcount www.youtube.com:443) -ge 1 ]"
http_proxy="http://chris:se%40cret@127.0.0.1:$PPORT" "$CLI" play 7PIMiDcwNvc 720 "$W/pplay.json" 2> "$W/err" || { cat "$W/err"; fail=1; }
check "http_proxy: the player is asked through it too" "grep -q '/itag/232/' '$W/pplay.json' && [ \$(pcount youtubei.googleapis.com:443) -eq 1 ] && [ \$(pcount manifest.googlevideo.com:443) -eq 1 ]"
before=$(wc -l < "$W/plog")
http_proxy="http://chris:se%40cret@127.0.0.1:$PPORT" no_proxy=".youtube.com" "$CLI" search test > /dev/null 2> "$W/err" || { cat "$W/err"; fail=1; }
check "no_proxy: those hosts directly" "[ \$(wc -l < '$W/plog') -eq $before ]"
http_proxy="http://chris:wrong@127.0.0.1:$PPORT" "$CLI" search test > /dev/null 2> "$W/err" && fail=1
check "a wrong password: the proxy's refusal is reported" "grep -q 'refused: 407 Proxy Authentication Required' '$W/err'"
http_proxy="http://chris:se%40cret@127.0.0.1:$PPORT" "$CLI" -proxy none search test > /dev/null 2> "$W/err" || { cat "$W/err"; fail=1; }
check "-proxy none: no proxy" "! grep -q 'through the proxy' '$W/err'"
"$CLI" -proxy "http://127.0.0.1:1" search test > /dev/null 2> "$W/err" && fail=1
check "a proxy that isn't there: said so" "grep -q '127.0.0.1:1' '$W/err'"
http_proxy="127.0.0.1:$PPORT" "$CLI" search test > /dev/null 2> "$W/err" && fail=1
check "http_proxy without http:// (Reel would ignore it): said so" "grep -q 'should start http://' '$W/err'"
http_proxy="http://chris:se%40cret@127.0.0.1" "$CLI" search test > /dev/null 2> "$W/err" && fail=1
check "http_proxy without a port (Reel would fail): said so, no password shown" "grep -q 'needs the proxy' '$W/err' && ! grep -q 'cret' '$W/err'"
check "every request through the proxy as YouTube expects" "python3 -I -c \"
import json
bad=[r for r in (json.loads(l) for l in open('$W/log')) if r.get('problems')]
assert not bad, bad\""

[ $fail = 0 ] && echo "e2e: all passed" || echo "e2e: FAILED"
exit $fail
