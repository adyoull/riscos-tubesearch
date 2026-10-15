#!/usr/bin/env python3
"""A stand-in for YouTube's Innertube API over HTTPS, for tests/e2e.sh.

Answers with the recorded responses in tests/fixtures (keep-alive,
alternately chunked and with Content-Length), checks each request the way
YouTube would care about (JSON body, client name and version, visitor
data once it has been handed out), and logs every request as a line of
JSON to the log file.

  fake_youtube.py PORT CERT KEY FIXTURE_DIR LOG [EJS_DIR]
EJS_DIR: manifest.json and VERSION/yt.solver.*.min.js, served as
raw.githubusercontent.com and GitHub's release downloads (with GitHub's
redirect to its asset server).
Part of TubeSearch. MIT licence."""
import os, json, ssl, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT, CERT, KEY, FIX, LOG = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
EJS = sys.argv[6] if len(sys.argv) > 6 else '/nonexistent'
HLS_MASTER = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'hls', 'master.m3u8')
lock = threading.Lock()
count = [0]
VISITOR = json.load(open(FIX + '/visitor.json'))['responseContext']['visitorData']


def fixture(name):
    return open(FIX + '/' + name, 'rb').read()


class H(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *a):
        pass

    def send_body(self, code, body, ctype='application/json; charset=UTF-8'):
        with lock:
            count[0] += 1
            chunked = count[0] % 2 == 0
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        if chunked:
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            for i in range(0, len(body), 7000):
                part = body[i:i + 7000]
                self.wfile.write(b'%x\r\n' % len(part) + part + b'\r\n')
            self.wfile.write(b'0\r\n\r\n')
        else:
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    def record(self, entry):
        with lock, open(LOG, 'a') as f:
            f.write(json.dumps(entry) + '\n')

    def do_GET(self):
        host = self.headers.get('Host')
        self.record({'method': 'GET', 'path': self.path, 'host': host})
        if host == 'manifest.googlevideo.com' and self.path.startswith('/api/manifest/hls_variant/'):
            # the HLS master playlist (a synthetic one in YouTube's form)
            self.send_body(200, open(HLS_MASTER, 'rb').read(), 'application/vnd.apple.mpegurl')
        elif host == 'i.ytimg.com' and self.path.endswith('/mqdefault.jpg'):
            self.send_body(200, fixture('thumb.jpg'), 'image/jpeg')
        elif host == 'www.youtube.com' and self.path == '/iframe_api':
            self.send_body(200, fixture('iframe_api.js'), 'text/javascript')
        elif host == 'www.youtube.com' and self.path == '/s/player/854a788e/player_ias.vflset/en_US/base.js':
            self.send_body(200, fixture('player_854a788e.js'), 'text/javascript')
        elif host == 'raw.githubusercontent.com' and self.path == '/adyoull/riscos-tubesearch/main/ejs.json':
            try:
                self.send_body(200, open(EJS + '/manifest.json', 'rb').read(), 'text/plain')
            except OSError:
                self.send_body(404, b'404: Not Found', 'text/plain')
        elif host == 'github.com' and self.path.startswith('/yt-dlp/ejs/releases/download/'):
            # as GitHub does: a redirect to the asset server
            ver, name = self.path.split('/')[-2:]
            self.send_response(302)
            self.send_header('Location', 'https://release-assets.githubusercontent.com/github-production-release-asset/ejs/%s/%s?sp=r&sig=x' % (ver, name))
            self.send_header('Content-Length', '0')
            self.end_headers()
        elif host == 'release-assets.githubusercontent.com':
            ver, name = self.path.split('?')[0].split('/')[-2:]
            try:
                self.send_body(200, open('%s/%s/%s' % (EJS, ver, name), 'rb').read(), 'application/octet-stream')
            except OSError:
                self.send_body(404, b'Not Found', 'text/plain')
        else:
            self.send_body(404, b'not here', 'text/plain')

    def do_POST(self):
        n = int(self.headers.get('Content-Length', '0'))
        raw = self.rfile.read(n)
        problems = []
        try:
            body = json.loads(raw)
        except Exception as e:
            body = {}
            problems.append('body is not JSON: %s' % e)
        client = body.get('context', {}).get('client', {})
        path = self.path.split('?')[0]
        ep = path.rsplit('/', 1)[-1]
        host = self.headers.get('Host')
        tv = ep == 'player' and client.get('clientName') == 'TVHTML5'
        if tv:
            if host != 'www.youtube.com':
                problems.append('TV player request to %s' % host)
            if self.headers.get('X-YouTube-Client-Name') != '7':
                problems.append('TV request without X-YouTube-Client-Name 7')
            sts = body.get('playbackContext', {}).get('contentPlaybackContext', {}).get('signatureTimestamp')
            if sts != 20668:
                problems.append('TV request with signatureTimestamp %r' % sts)
        elif ep == 'player':
            if host != 'youtubei.googleapis.com':
                problems.append('player request to %s' % host)
            if client.get('clientName') != 'VISIONOS':
                problems.append('player client %s' % client.get('clientName'))
            if 'id=' + body.get('videoId', '?') not in self.path:
                problems.append('player path without id')
        else:
            if host != 'www.youtube.com':
                problems.append('%s request to %s' % (ep, host))
            if self.headers.get('X-YouTube-Client-Name') != '1':
                problems.append('no X-YouTube-Client-Name')
            if client.get('clientName') != 'WEB':
                problems.append('client %s' % client.get('clientName'))
        if ep != 'visitor_id' and client.get('visitorData') != VISITOR:
            problems.append('no visitorData')
        self.record({'method': 'POST', 'endpoint': 'player-tv' if tv else ep, 'host': host, 'problems': problems,
                     'continuation': 'continuation' in body, 'query': body.get('query'),
                     'params': body.get('params')})
        if problems:
            self.send_body(400, json.dumps({'error': {'code': 400, 'message': '; '.join(problems)}}).encode())
            return
        cont = 'continuation' in body
        name = {'visitor_id': 'visitor.json',
                'search': 'search_2.json' if cont else 'search_1.json',
                'next': 'next.json',
                'browse': 'channel_2.json' if cont else 'channel_1.json',
                'player': 'player_visionos.json'}.get(ep)
        vid = body.get('videoId')
        if ep == 'player' and vid not in ('7PIMiDcwNvc', 'BotCheck001'):
            self.send_body(200, b'{"playabilityStatus":{"status":"ERROR","reason":"Video unavailable"}}')
        elif tv:
            self.send_body(200, fixture('player_tv.json'))
        elif ep == 'player' and vid == 'BotCheck001':
            # what YouTube says when it won't serve a client: the TV fallback's cue
            self.send_body(200, b'{"playabilityStatus":{"status":"LOGIN_REQUIRED","reason":"Sign in to confirm you\\u2019re not a bot"}}')
        elif not name:
            self.send_body(404, b'{"error":{"code":404,"message":"no such endpoint"}}')
        else:
            self.send_body(200, fixture(name))


ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(CERT, KEY)
srv = ThreadingHTTPServer(('127.0.0.1', PORT), H)
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
print('ready', flush=True)
srv.serve_forever()
