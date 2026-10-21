# SPDX-License-Identifier: MIT
"""Real host process/socket tests; synthetic source, NOT LG playback validation."""
import http.client
import json
import math
import os
from pathlib import Path
import signal
import socket
import stat
import struct
import sys
import wave
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get('AUDIOCAST_TEST_BINARY', ROOT / 'build/native/audiocastd'))
RUNNER = os.environ.get('AUDIOCAST_TEST_RUNNER', '').split()


class DaemonTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='audiocast-test-')
        cls.state = Path(cls.tmp.name) / 'state'
        cls.proc = subprocess.Popen(RUNNER + [str(BINARY), '--state-dir', str(cls.state),
                                              '--allow-test-source'], stdout=subprocess.DEVNULL,
                                    stderr=subprocess.PIPE)
        for _ in range(200):
            try:
                cls.credentials = json.loads((cls.state / 'credentials.json').read_text())
                cls.api('GET', '/v1/status')
                break
            except (OSError, ValueError, http.client.HTTPException):
                if cls.proc.poll() is not None:
                    raise RuntimeError(cls.proc.stderr.read().decode())
                time.sleep(.03)
        else:
            raise RuntimeError('daemon did not start')

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        try:
            cls.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            cls.proc.kill()
            raise
        cls.proc.stderr.close()
        cls.tmp.cleanup()

    @classmethod
    def api(cls, method, path, body=None, token=None, port=7071, expected=200):
        conn = http.client.HTTPConnection('127.0.0.1', port, timeout=3)
        headers = {'Authorization': 'Bearer ' + (cls.credentials['control_token'] if token is None else token)}
        payload = None
        if body is not None:
            payload = json.dumps(body)
            headers['Content-Type'] = 'application/json'
        conn.request(method, path, payload, headers)
        response = conn.getresponse()
        data = response.read()
        conn.close()
        if response.status != expected:
            raise AssertionError((response.status, data, expected))
        return json.loads(data) if data else None

    def setUp(self):
        self.api('POST', '/v1/stop', {})
        # Stop intent is synchronous, resource release takes at most one capture read.
        for _ in range(100):
            try:
                self.api('POST', '/v1/config', {
                    'backend': 'test-tone', 'device': '', 'transport': 'rtp',
                    'sample_format': 'S16_LE', 'rate': 48000, 'channels': 2,
                    'destination': '127.0.0.1', 'destination_port': 5004,
                    'packet_ms': 10, 'profile': 'low-latency', 'http_lan': False,
                    'autostart': False, 'playback_confirmed': False})
                break
            except AssertionError:
                time.sleep(.02)
        else:
            self.fail('capture did not release resources')

    def wait_state(self, name, timeout=3):
        until = time.monotonic() + timeout
        while time.monotonic() < until:
            status = self.api('GET', '/v1/status')
            if status['state'] == name:
                return status
            time.sleep(.02)
        self.fail('state not reached: ' + name)

    def start(self, **changes):
        self.api('POST', '/v1/config', dict(playback_confirmed=True, **changes))
        self.api('POST', '/v1/start', {})
        return self.wait_state('capturing')

    def stream(self):
        c = http.client.HTTPConnection('127.0.0.1', 7070, timeout=3)
        c.request('GET', '/audio', headers={'Authorization': 'Bearer ' + self.credentials['stream_token']})
        r = c.getresponse()
        self.assertEqual(r.status, 200)
        return c, r

    def test_authentication_and_secret_separation(self):
        self.api('GET', '/v1/status', token='bad', expected=401)
        self.api('GET', '/v1/status', token=self.credentials['stream_token'], expected=401)
        self.api('GET', '/metadata', token=self.credentials['control_token'], port=7070, expected=401)
        result = self.api('GET', '/metadata', token=self.credentials['stream_token'], port=7070)
        self.assertEqual(result['sample_format'], 'S16_LE')
        self.api('GET', '/v1/status', token=self.credentials['stream_token'], port=7070, expected=404)
        diagnostic = json.dumps(self.api('GET', '/v1/diagnostics'))
        for token in self.credentials.values():
            self.assertNotIn(token, diagnostic)
        self.assertEqual(stat.S_IMODE(self.state.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE((self.state / 'credentials.json').stat().st_mode), 0o600)

    def test_invalid_config_is_atomic_and_fail_closed(self):
        before = self.api('GET', '/v1/config')
        for invalid in [{'destination': 'x; touch /tmp/no'}, {'destination_port': -1},
                        {'destination_port': 0}, {'channels': 0}, {'rate': 48000.5},
                        {'channels': True}, {'unknown': 1}, {'device': 'default'},
                        {'backend': 'microphone'}, {'sample_format': 'S32_LE'},
                        {'packet_ms': 7}, {'rate': 8000}, {'autostart': 'yes'}]:
            self.api('POST', '/v1/config', invalid, expected=400)
            self.assertEqual(self.api('GET', '/v1/config'), before)
        self.api('POST', '/v1/start', {}, expected=400)
        self.assertFalse(self.api('GET', '/v1/status')['streaming'])

    def test_rtp_endian_packetization_timestamps_real_levels(self):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(('127.0.0.1', 0))
        sock.settimeout(2)
        try:
            self.start(destination_port=sock.getsockname()[1])
            packets = [sock.recv(2048) for _ in range(12)]
            samples = bytearray()
            for i, p in enumerate(packets):
                self.assertLessEqual(len(p), 1200)
                self.assertEqual(p[0], 0x80)
                self.assertEqual(p[1] & 0x7f, 96)
                self.assertEqual((len(p) - 12) % 4, 0)
                samples.extend(p[12:])
                if i:
                    old = packets[i-1]
                    self.assertEqual((struct.unpack('!H', p[2:4])[0] - struct.unpack('!H', old[2:4])[0]) & 65535, 1)
                    self.assertEqual((struct.unpack('!I', p[4:8])[0] - struct.unpack('!I', old[4:8])[0]) & 0xffffffff, (len(old)-12)//4)
                    self.assertEqual(p[8:12], old[8:12])
            values = struct.unpack('!' + 'h'*(len(samples)//2), samples)
            self.assertEqual(values[0::2], values[1::2])
            self.assertLessEqual(max(abs(v) for v in values), 8192)
            rms = math.sqrt(sum(v*v for v in values)/len(values))/32768
            self.assertAlmostEqual(rms, .25/math.sqrt(2), delta=.01)
            status = self.api('GET', '/v1/status')
            self.assertEqual(status['signal'], 'detected')
            self.assertGreater(status['levels']['rms'][0], .15)
            self.assertIsNone(status['end_to_end_ms'])
            self.assertIsNone(status['rtp_receivers'])
        finally:
            sock.close()

    def test_rtcp_sender_report_sdes_and_bye(self):
        rtp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        rtcp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        for _ in range(20):
            rtp.bind(('127.0.0.1',0));port=rtp.getsockname()[1]
            try:
                if port==65535:raise OSError('reserved upper bound')
                rtcp.bind(('127.0.0.1',port+1));break
            except OSError:
                rtp.close();rtp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        else:self.fail('could not reserve RTP/RTCP pair')
        rtp.settimeout(2);rtcp.settimeout(2)
        try:
            self.start(destination_port=port)
            audio=rtp.recv(2048);report=rtcp.recv(2048)
            self.assertEqual(report[0:4],bytes([0x80,200,0,6]))
            self.assertEqual(report[4:8],audio[8:12])
            self.assertGreater(struct.unpack('!I',report[8:12])[0],2208988800)
            self.assertGreater(struct.unpack('!I',report[20:24])[0],0)
            self.assertEqual(report[28:30],bytes([0x81,202]))
            self.assertIn(b'audiocast-',report)
            self.api('POST','/v1/stop',{})
            goodbye=rtcp.recv(2048)
            self.assertEqual(goodbye[-8:-4],bytes([0x81,203,0,1]))
        finally:rtp.close();rtcp.close()

    def test_http_multiclient_raw_framing(self):
        self.start(transport='http')
        pairs = [self.stream(), self.stream()]
        try:
            for _, r in pairs:
                self.assertEqual(r.getheader('X-Audio-Rate'), '48000')
                self.assertEqual(r.getheader('X-Audio-Channels'), '2')
                self.assertEqual(r.getheader('X-Audio-Format'), 'S16_LE')
                data = r.read(19200)
                values = struct.unpack('<' + 'h'*(len(data)//2), data)
                self.assertEqual(values[::2], values[1::2])
                self.assertGreater(max(values), 7000)
            self.assertEqual(self.api('GET', '/v1/status')['http_clients'], 2)
        finally:
            for c, r in pairs:
                r.close(); c.close()

    def test_slow_http_receiver_does_not_backpressure_healthy_client(self):
        self.start(transport='both')
        slow = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        slow.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
        slow.connect(('127.0.0.1', 7070))
        slow.sendall(('GET /audio HTTP/1.1\r\nHost: localhost\r\nAuthorization: Bearer ' +
                      self.credentials['stream_token'] + '\r\n\r\n').encode())
        c, r = self.stream()
        before = self.api('GET', '/v1/status')
        try:
            for _ in range(18):
                self.assertEqual(len(r.read(19200)), 19200)
            after = self.api('GET', '/v1/status')
            self.assertGreater(after.get('http_slow_disconnects', 0), before.get('http_slow_disconnects', 0))
            self.assertGreater(after['rtp_packets'], before['rtp_packets'] + 100)
            self.assertEqual(after['state'], 'capturing')
        finally:
            slow.close(); r.close(); c.close()

    def test_s32_http_mono_no_conversion(self):
        self.start(transport='http', sample_format='S32_LE', channels=1)
        c, r = self.stream()
        try:
            data = r.read(4096)
            values = struct.unpack('<1024i', data)
            self.assertGreater(max(values), 500000000)
            self.assertLessEqual(max(values), 536870912)
        finally:
            r.close(); c.close()

    def test_test_capture_never_streams_and_expires(self):
        self.api('POST', '/v1/test-capture', {})
        self.wait_state('capturing')
        self.api('GET', '/audio', port=7070, token=self.credentials['stream_token'], expected=409)
        self.assertTrue(self.api('GET', '/v1/status')['test_active'])
        self.wait_state('idle', timeout=6)
        self.assertFalse(self.api('GET', '/v1/status')['test_active'])

    def test_probe_honesty_private_destinations_only(self):
        result = self.api('POST', '/v1/test-connection', {})
        self.assertIsNone(result['reachable'])
        self.assertFalse(result['delivery_verified'])
        self.api('POST', '/v1/config', {'destination': '8.8.8.8'})
        self.api('POST', '/v1/test-connection', {}, expected=400)

    def test_reference_receiver_rtp_and_explicit_recording(self):
        port_socket=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        port_socket.bind(('127.0.0.1',0));port=port_socket.getsockname()[1];port_socket.close()
        recording=Path(self.tmp.name)/'receiver-rtp.wav'
        process=subprocess.Popen([sys.executable,str(ROOT/'receiver/receiver.py'),'rtp',
            '--bind','127.0.0.1','--port',str(port),'--duration','1',
            '--record',str(recording)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        time.sleep(.15)
        self.start(destination_port=port)
        out,err=process.communicate(timeout=5)
        self.assertEqual(process.returncode,0,err)
        final=json.loads(out.strip().splitlines()[-1])['final']
        self.assertGreater(final['received_pcm_bytes'],96000)
        # Scheduler jitter on a shared host is not deterministic; deterministic loss accounting
        # is covered in test_receiver.py. Integration must report losses, not fabricate zero.
        self.assertIsInstance(final['missing_frames'],int)
        self.assertGreaterEqual(final['missing_frames'],0)
        with wave.open(str(recording),'rb') as wav:
            self.assertEqual(wav.getframerate(),48000)
            self.assertGreater(wav.getnframes(),24000)
        recording.unlink()

    def test_reference_receiver_http_and_authentication(self):
        self.start(transport='http')
        token=Path(self.tmp.name)/'receiver-token'
        token.write_text(self.credentials['stream_token']);token.chmod(0o600)
        try:
            p=subprocess.run([sys.executable,str(ROOT/'receiver/receiver.py'),'http',
                '--url','http://127.0.0.1:7070/audio','--token-file',str(token),
                '--duration','.3'],capture_output=True,text=True,timeout=6)
            self.assertEqual(p.returncode,0,p.stderr)
            final=json.loads(p.stdout.strip().splitlines()[-1])['final']
            self.assertGreater(final['received_pcm_bytes'],20000)
            token.write_text('bad-token')
            p=subprocess.run([sys.executable,str(ROOT/'receiver/receiver.py'),'http',
                '--url','http://127.0.0.1:7070/audio','--token-file',str(token),
                '--duration','.1'],capture_output=True,text=True,timeout=6)
            self.assertNotEqual(p.returncode,0)
            self.assertIn('401',p.stderr)
        finally:token.unlink()

    def test_suspend_resume_and_no_config_mutation_while_active(self):
        self.start()
        self.api('POST', '/v1/config', {'rate': 44100}, expected=400)
        self.api('POST', '/v1/suspend', {})
        self.wait_state('paused')
        self.assertFalse(self.api('GET', '/v1/status')['streaming'])
        self.api('POST', '/v1/resume', {})
        self.wait_state('capturing')

    def test_duplicate_instance_cannot_replace_pid(self):
        before = (self.state / 'daemon.pid').read_text()
        p = subprocess.run(RUNNER + [str(BINARY), '--state-dir', str(self.state), '--allow-test-source'],
                           capture_output=True, timeout=5)
        self.assertNotEqual(p.returncode, 0)
        self.assertEqual((self.state / 'daemon.pid').read_text(), before)

    def test_device_change_invalidates_confirmation(self):
        self.api('POST', '/v1/config', {'playback_confirmed': True})
        config = self.api('POST', '/v1/config', {'device': 'hw:0,13'})
        self.assertFalse(config['playback_confirmed'])

    def test_http_parser_rejects_ambiguous_framing(self):
        for headers in [b'Content-Length: 0\r\nContent-Length: 0',
                        b'Transfer-Encoding: chunked', b'Content-Length: -1']:
            with socket.create_connection(('127.0.0.1', 7071), timeout=2) as s:
                s.sendall(b'POST /v1/start HTTP/1.1\r\nHost: localhost\r\n' + headers + b'\r\n\r\n')
                self.assertIn(b' 400 ', s.recv(2048))


if __name__ == '__main__':
    unittest.main()
