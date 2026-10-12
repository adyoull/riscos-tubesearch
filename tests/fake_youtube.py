#!/usr/bin/env python3
"""A stand-in for YouTube's Innertube API over HTTPS, for tests/e2e.sh.

Answers with the recorded responses in tests/fixtures (keep-alive,
alternately chunked and with Content-Length), checks each request the way
YouTube would care about (JSON body, client name and version, visitor
data once it has been handed out), and logs every request as a line of
JSON to the log file.

  fake_youtube.py PORT CERT KEY FIXTURE_DIR LOG
Part of TubeSearch. MIT licence."""
import json, ssl, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT, CERT, KEY, FIX, LOG = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
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
        self.record({'method': 'GET', 'path': self.path, 'host': self.headers.get('Host')})
        if self.path.startswith('/vi/') and self.path.endswith('/mqdefault.jpg'):
            self.send_body(200, fixture('thumb.jpg'), 'image/jpeg')
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
        if ep == 'player':
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
        self.record({'method': 'POST', 'endpoint': ep, 'host': host, 'problems': problems,
                     'continuation': 'continuation' in body, 'query': body.get('query')})
        if problems:
            self.send_body(400, json.dumps({'error': {'code': 400, 'message': '; '.join(problems)}}).encode())
            return
        cont = 'continuation' in body
        name = {'visitor_id': 'visitor.json',
                'search': 'search_2.json' if cont else 'search_1.json',
                'next': 'next.json',
                'browse': 'channel_2.json' if cont else 'channel_1.json',
                'player': 'player_visionos.json'}.get(ep)
        if ep == 'player' and body.get('videoId') != '7PIMiDcwNvc':
            self.send_body(200, b'{"playabilityStatus":{"status":"ERROR","reason":"Video unavailable"}}')
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
