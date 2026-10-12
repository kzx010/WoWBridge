"""OBS transport regressions. No desktop, credentials or live OBS required."""
import importlib.util
import json
from pathlib import Path
import struct
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('tested_obs', Path(__file__).parents[1] / 'obs.py')
obs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(obs)


def server_frame(value):
    data = json.dumps(value).encode()
    return b'\x81' + bytes([len(data)]) + data


class Socket:
    def __init__(self, data):
        self.data, self.sent = data, []

    def recv(self, n):
        out, self.data = self.data[:n], self.data[n:]
        return out

    def sendall(self, data):
        self.sent.append(data)


class OBSProtocol(unittest.TestCase):
    def test_hello_in_upgrade_packet_is_not_discarded(self):
        sock = Socket(b'HTTP/1.1 101 Switching Protocols\r\n\r\n' +
                      server_frame({'op': 0, 'd': {'rpcVersion': 1}}) +
                      server_frame({'op': 2, 'd': {}}))
        with patch.object(obs.socket, 'create_connection', return_value=sock):
            self.assertIs(obs.connect(4455, ''), sock)
        self.assertEqual(len(sock.sent), 2)

    def test_closed_upgrade_fails_without_looping(self):
        with patch.object(obs.socket, 'create_connection', return_value=Socket(b'HTTP/1.1')):
            with self.assertRaisesRegex(ConnectionError, 'closed during'):
                obs.connect(4455, '')

    def test_oversized_upgrade_is_bounded(self):
        with patch.object(obs.socket, 'create_connection', return_value=Socket(b'x' * 16385)):
            with self.assertRaisesRegex(ConnectionError, 'oversized'):
                obs.connect(4455, '')

    def test_request_data_and_unsolicited_event(self):
        result = {'op': 7, 'd': {'requestId': 'SetRecordDirectory', 'responseData': {}}}
        sock = Socket(server_frame({'op': 5, 'd': {}}) + server_frame(result))
        data = {'recordDirectory': 'local/demo'}
        self.assertEqual(obs.request(sock, 'SetRecordDirectory', data), result['d'])
        wire = sock.sent[0]
        n, at = wire[1] & 127, 2
        if n == 126:
            n, at = struct.unpack('>H', wire[2:4])[0], 4
        mask, payload = wire[at:at+4], wire[at+4:at+4+n]
        decoded = json.loads(bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))
        self.assertEqual(decoded['d']['requestData'], data)


if __name__ == '__main__':
    unittest.main()
