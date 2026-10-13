#!/usr/bin/env python3
"""A web proxy for the tests: answers CONNECT, then joins the client to the
fake YouTube (whatever host was asked for, as TUBE_CONNECT does without a
proxy). It wants Proxy-Authorization for USER:PASSWORD, and logs each
CONNECT as a JSON line.
  fake_proxy.py PORT TARGET_PORT USER:PASSWORD LOG
Part of TubeSearch. MIT licence."""
import base64, json, socket, sys, threading

PORT, TARGET, CRED, LOG = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]
lock = threading.Lock()


def pipe(a, b):
    try:
        while True:
            d = a.recv(65536)
            if not d:
                break
            b.sendall(d)
    except OSError:
        pass
    for s in (a, b):
        try:
            s.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


def handle(c):
    head = b''
    while b'\r\n\r\n' not in head:
        d = c.recv(1)                     # (byte by byte: nothing after the header is taken)
        if not d:
            c.close()
            return
        head += d
    lines = head.decode('latin-1').split('\r\n')
    method, target = lines[0].split(' ')[:2]
    headers = dict(l.split(': ', 1) for l in lines[1:] if ': ' in l)
    auth = headers.get('Proxy-Authorization', '')
    ok = auth == 'Basic ' + base64.b64encode(CRED.encode()).decode()
    with lock, open(LOG, 'a') as f:
        f.write(json.dumps({'method': method, 'target': target, 'host': headers.get('Host'), 'auth_ok': ok}) + '\n')
    if method != 'CONNECT':
        c.sendall(b'HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 0\r\n\r\n')
        c.close()
        return
    if not ok:
        c.sendall(b'HTTP/1.1 407 Proxy Authentication Required\r\nProxy-Authenticate: Basic realm="t"\r\n'
                  b'Content-Length: 4\r\n\r\nno!\n')
        c.close()
        return
    s = socket.create_connection(('127.0.0.1', TARGET))
    c.sendall(b'HTTP/1.1 200 Connection established\r\nProxy-Agent: fake\r\n\r\n')
    threading.Thread(target=pipe, args=(c, s), daemon=True).start()
    pipe(s, c)


srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', PORT))
srv.listen(16)
print('ready', flush=True)
while True:
    conn, _ = srv.accept()
    threading.Thread(target=handle, args=(conn,), daemon=True).start()
