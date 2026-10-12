#!/bin/sh
# End to end on Linux: tubecli (built with OpenSSL) against a local HTTPS
# stand-in for YouTube (fake_youtube.py) answering with the recorded
# fixtures. Checks the lists, the chosen streams and every request.
#   tests/e2e.sh TUBECLI
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
CLI=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
W=$(mktemp -d)
trap 'kill $SRV 2>/dev/null || true; rm -rf "$W"' EXIT
FIX=$HERE/fixtures
[ -s "$FIX/next.json" ] || { echo "no fixtures: run tests/fetch-fixtures.sh" >&2; exit 1; }
python3 -I -c "
from PIL import Image
Image.new('RGB', (320, 180), (200, 30, 30)).save('$FIX/thumb.jpg', quality=80)"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$W/key.pem" -out "$W/cert.pem" \
  -days 2 -subj /CN=www.youtube.com >/dev/null 2>&1
PORT=$((20000 + $$ % 20000))
python3 -I "$HERE/fake_youtube.py" $PORT "$W/cert.pem" "$W/key.pem" "$FIX" "$W/log" > "$W/srv.out" 2>&1 &
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
check "play: 720p H.264 + AAC for Reel" "grep -q '720p avc1' '$W/err' && python3 -I -c \"
import json,sys
d=json.load(open('$W/play.json'))
f=d['requested_formats']
assert 'itag=136' in f[0]['url'] and f[0]['acodec']=='none'
assert 'itag=140' in f[1]['url'] and f[1]['vcodec']=='none'
assert d['webpage_url'].endswith('7PIMiDcwNvc')
\""
check "every request as YouTube expects" "python3 -I -c \"
import json
rows=[json.loads(l) for l in open('$W/log')]
bad=[r for r in rows if r.get('problems')]
assert not bad, bad
eps=[r.get('endpoint') for r in rows]
assert eps.count('visitor_id')==4, eps
assert eps.count('player')==1 and eps.count('search')==2 and eps.count('browse')==2 and eps.count('next')==1, eps
\""
"$CLI" play badvideoid 720 > /dev/null 2> "$W/err" && fail=1
check "an unplayable answer is reported" "grep -q 'tubecli:' '$W/err'"
[ $fail = 0 ] && echo "e2e: all passed" || echo "e2e: FAILED"
exit $fail
