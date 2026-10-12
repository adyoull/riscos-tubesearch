#!/usr/bin/env python3
"""Keeps ejs.json (which TubeSearch reads once a day) at the solver version
yt-dlp itself pins.

  tools/update-ejs-manifest.py [--info _info.py] [--manifest ejs.json]
                               [--player tests/fixtures/player_854a788e.js]

Reads VERSION and HASHES from yt-dlp's
yt_dlp/extractor/youtube/jsc/_builtin/vendor/_info.py (--info, or the
installed yt_dlp). If they differ from ejs.json, it downloads that version's
yt.solver.lib.min.js and yt.solver.core.min.js from yt-dlp/ejs's GitHub
releases, checks their SHA3-512 against yt-dlp's hashes, and (with
--player and node 22+) checks they still answer known challenges for a
recorded player. Only then is ejs.json rewritten. Exit status 0 either way
(1 if a check failed); "changed=true|false" goes to $GITHUB_OUTPUT.

Part of TubeSearch. MIT licence."""
import argparse, hashlib, json, os, re, subprocess, sys, tempfile, urllib.request

NAMES = ('yt.solver.lib.min.js', 'yt.solver.core.min.js')
RELEASES = 'https://github.com/yt-dlp/ejs/releases/download'
# player 854a788e (NewPipe's recording): challenges and the answers node gives
CHALLENGES = {
    'n': {'IlLiA21ny7gqA2m4p37': 'RL7twTMlYAhbWlqMF', 'eabGFpsUKuWHXGh6FR4': 'OtjoLvmzT_c2ob_ei'},
    'sig': {'NJAJEij0EwRgIhAI0KExTgjfPk-MPM9MAdzyyPRt=BM8-XO5tm5hlMCSVpAiEAv7eP3CURqZNSPow8BXXAoazVoXgeMP7gH9BdylHCwgw=gwzz':
            'zzwg=wgwCHlydB9Hg7PMegXoVzaoAXXB8woPSNZqRUC3Pe7vAEiApVSCMlh5mt5OX-NMB=tRPyyzdAM9MPM-kPfjgTxEK0IAhIgRwE0jiEJA'},
}


def output(**kv):
    path = os.environ.get('GITHUB_OUTPUT')
    if path:
        with open(path, 'a') as f:
            for k, v in kv.items():
                f.write(f'{k}={v}\n')


def ytdlp_pins(info_path):
    if info_path:
        src = open(info_path).read()
        version = re.search(r"VERSION = '([^']+)'", src).group(1)
        hashes = dict(re.findall(r"'(yt\.solver\.[a-z.]+js)': '([0-9a-f]{128})'", src))
        ytdlp = 'yt-dlp (%s)' % info_path
    else:
        import yt_dlp.version
        from yt_dlp.extractor.youtube.jsc._builtin.vendor._info import HASHES as hashes, VERSION as version
        ytdlp = 'yt-dlp %s' % yt_dlp.version.__version__
    return version, {n: hashes[n] for n in NAMES}, ytdlp


def solves(code, player):
    lib, core = code
    data = {'type': 'player', 'player': open(player).read(),
            'requests': [{'type': t, 'challenges': list(c)} for t, c in CHALLENGES.items()],
            'output_preprocessed': False}
    with tempfile.NamedTemporaryFile('w', suffix='.js', delete=False) as f:
        f.write(f'{lib}\nObject.assign(globalThis, lib);\n{core}\n'
                f'console.log(JSON.stringify(jsc({json.dumps(data)})));\n')
    try:
        out = json.loads(subprocess.run(['node', f.name], capture_output=True, text=True, check=True).stdout)
    finally:
        os.unlink(f.name)
    if out.get('type') != 'result':
        return 'the solver: %s' % out.get('error')
    for (t, want), got in zip(CHALLENGES.items(), out['responses']):
        if got.get('data') != want:
            return '%s challenges answered wrongly: %s' % (t, got)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--info')
    ap.add_argument('--manifest', default='ejs.json')
    ap.add_argument('--player')
    a = ap.parse_args()
    version, hashes, ytdlp = ytdlp_pins(a.info)
    old = json.load(open(a.manifest))
    if old.get('version') == version and old.get('files') == hashes:
        print('ejs.json already names %s (as %s)' % (version, ytdlp))
        output(changed='false', version=version)
        return 0
    code = []
    for n in NAMES:
        url = '%s/%s/%s' % (RELEASES, version, n)
        data = urllib.request.urlopen(url, timeout=60).read()
        got = hashlib.sha3_512(data).hexdigest()
        if got != hashes[n]:
            print('FAIL: %s has SHA3-512 %s, yt-dlp pins %s' % (url, got, hashes[n]))
            output(changed='false')
            return 1
        code.append(data.decode('utf-8'))
    if a.player:
        problem = solves(code, a.player)
        if problem:
            print('FAIL: solver %s on the recorded player: %s' % (version, problem))
            output(changed='false')
            return 1
    new = {'version': version, 'files': hashes, 'source': ytdlp}
    with open(a.manifest, 'w') as f:
        f.write(json.dumps(new, indent=2) + '\n')
    print('ejs.json: %s -> %s (as %s)' % (old.get('version'), version, ytdlp))
    output(changed='true', version=version)
    return 0


if __name__ == '__main__':
    sys.exit(main())
