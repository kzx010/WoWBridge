#!/usr/bin/env python3
"""Start or stop an OBS recording over obs-websocket v5 (the user's OBS, already set to capture Skyrim).

usage: obs.py start | stop | status
Reads the port and password from OBS's own websocket config; launches OBS (minimized to its tray) if it is not
running. A test run records from before Skyrim starts until it has closed (tools/play.sh test). Stdlib only.
"""
import base64, hashlib, json, os, socket, struct, subprocess, sys, time
from pathlib import Path

CONFIG = Path.home() / '.config/obs-studio/plugin_config/obs-websocket/config.json'


def _frame(text: str) -> bytes:   # a masked client text frame
    data = text.encode()
    head = bytes([0x81])
    n = len(data)
    head += bytes([0x80 | n]) if n < 126 else bytes([0x80 | 126]) + struct.pack('>H', n) if n < 65536 else bytes([0x80 | 127]) + struct.pack('>Q', n)
    mask = os.urandom(4)
    return head + mask + bytes(b ^ mask[i % 4] for i, b in enumerate(data))


def _read(sock: socket.socket) -> dict:
    def exact(n):
        out = b''
        while len(out) < n:
            chunk = sock.recv(n - len(out))
            if not chunk:
                raise ConnectionError('closed')
            out += chunk
        return out
    b0, b1 = exact(2)
    n = b1 & 0x7F
    n = struct.unpack('>H', exact(2))[0] if n == 126 else struct.unpack('>Q', exact(8))[0] if n == 127 else n
    payload = exact(n)
    return json.loads(payload) if (b0 & 0x0F) == 1 else _read(sock)


def connect(port: int, password: str) -> socket.socket:
    sock = socket.create_connection(('127.0.0.1', port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode()
    sock.sendall((f'GET / HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                  f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: obswebsocket.json\r\n\r\n').encode())
    response = b''
    while b'\r\n\r\n' not in response:
        # Preserve Hello when OBS sends it in the HTTP upgrade packet.
        chunk = sock.recv(1)
        if not chunk:
            raise ConnectionError('closed during websocket upgrade')
        response += chunk
        if len(response) > 16384:
            raise ConnectionError('oversized websocket upgrade response')
    if b' 101 ' not in response.split(b'\r\n')[0]:
        raise ConnectionError(response.split(b'\r\n')[0].decode())
    hello = _read(sock)['d']
    identify = {'rpcVersion': 1}
    if 'authentication' in hello:   # base64(sha256(base64(sha256(password + salt)) + challenge))
        a = hello['authentication']
        secret = base64.b64encode(hashlib.sha256((password + a['salt']).encode()).digest()).decode()
        identify['authentication'] = base64.b64encode(hashlib.sha256((secret + a['challenge']).encode()).digest()).decode()
    sock.sendall(_frame(json.dumps({'op': 1, 'd': identify})))
    if _read(sock).get('op') != 2:
        raise ConnectionError('not identified')
    return sock


def request(sock: socket.socket, kind: str, data: dict | None = None) -> dict:
    body = {'requestType': kind, 'requestId': kind}
    if data is not None:
        body['requestData'] = data
    sock.sendall(_frame(json.dumps({'op': 6, 'd': body})))
    while True:
        msg = _read(sock)
        if msg.get('op') == 7 and msg['d'].get('requestId') == kind:
            return msg['d']


def main(what: str) -> int:
    cfg = json.loads(CONFIG.read_text())
    if not cfg.get('server_enabled'):
        print('obs: its websocket server is off (OBS > Tools > WebSocket Server Settings)')
        return 1
    for attempt in range(40):
        try:
            sock = connect(cfg.get('server_port', 4455), cfg.get('server_password', ''))
            break
        except OSError:
            if attempt == 0 and subprocess.run(['pgrep', '-x', 'obs'], capture_output=True).returncode != 0:
                subprocess.Popen(['obs', '--minimize-to-tray'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
            time.sleep(0.5)
    else:
        print('obs: not reachable')
        return 1
    status = request(sock, 'GetRecordStatus')
    active = status.get('responseData', {}).get('outputActive', False)
    if what == 'start' and not active:
        print('obs:', request(sock, 'StartRecord')['requestStatus'])
    elif what == 'stop' and active:
        result = request(sock, 'StopRecord')
        print('obs:', result['requestStatus'], result.get('responseData', {}).get('outputPath', ''))
    else:
        print('obs: recording' if active else 'obs: not recording')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else 'status'))
