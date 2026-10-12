#!/bin/sh
# Fetches real YouTube answers for the parser tests from NewPipe's recorded
# test data (github.com/TeamNewPipe/NewPipeExtractor, GPL-3.0; the answers
# themselves are YouTube's) and writes their bodies into tests/fixtures/.
# The fixtures are not kept in git (tests/fixtures is ignored).
#   tests/fetch-fixtures.sh [NEWPIPE_CHECKOUT]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/fixtures
COMMIT=3313901108f0f52df68c28a04d88c2891f9c400b      # 2026-10-11
NP=${1:-}
M=extractor/src/test/resources/mocks/v1/org/schabi/newpipe/extractor/services/youtube
if [ -z "$NP" ]; then
  NP=$HERE/.newpipe
  if [ ! -d "$NP/.git" ]; then
    git clone -q --filter=blob:none --no-checkout https://github.com/TeamNewPipe/NewPipeExtractor.git "$NP"
  fi
  git -C "$NP" fetch -q --depth 1 origin "$COMMIT" 2>/dev/null || true
  git -C "$NP" sparse-checkout init --cone
  git -C "$NP" sparse-checkout set \
    $M/search/youtubesearchextractor/all \
    $M/search/youtubesearchextractor/channel \
    $M/stream/youtubestreamextractordefault/descriptionpewdiepie \
    $M/youtubechanneltabextractor/videos
  git -C "$NP" checkout -q "$COMMIT"
fi
mkdir -p "$OUT"
python3 -I - "$NP/$M" "$OUT" <<'EOF'
import json, sys, os
src, out = sys.argv[1], sys.argv[2]
want = {
    'search_1.json':        'search/youtubesearchextractor/all/generated_mock_2.json',
    'search_2.json':        'search/youtubesearchextractor/all/generated_mock_3.json',
    'search_channel.json':  'search/youtubesearchextractor/channel/generated_mock_2.json',
    'visitor.json':         'stream/youtubestreamextractordefault/descriptionpewdiepie/generated_mock_4.json',
    'player_visionos.json': 'stream/youtubestreamextractordefault/descriptionpewdiepie/generated_mock_1.json',
    'next.json':            'stream/youtubestreamextractordefault/descriptionpewdiepie/generated_mock_6.json',
    'channel_1.json':       'youtubechanneltabextractor/videos/generated_mock_3.json',
    'channel_2.json':       'youtubechanneltabextractor/videos/generated_mock_4.json',
}
for name, rel in want.items():
    d = json.load(open(os.path.join(src, rel)))
    body = d['response']['responseBody']
    open(os.path.join(out, name), 'w').write(body)
    print(name, len(body), 'bytes from', d['request']['url'][:70])
EOF
