# SPDX-License-Identifier: MIT
"""ALSA ABI double tests, not hardware tests."""
import http.client
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/native/audiocastd'
LIBDIR = ROOT / 'build/test-lib'

class AlsaBackendTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        LIBDIR.mkdir(parents=True, exist_ok=True)
        subprocess.run(['gcc','-shared','-fPIC','-Wall','-Wextra','-Werror',
                        str(ROOT/'tests/fake_alsa.c'),'-o',str(LIBDIR/'libasound.so.2')], check=True)

    def api(self, method, endpoint, body=None):
        c = http.client.HTTPConnection('127.0.0.1', 7071, timeout=2)
        c.request(method, endpoint, json.dumps(body) if body is not None else None,
                  {'Authorization':'Bearer '+self.token})
        r = c.getresponse(); data=json.loads(r.read()); c.close()
        self.assertEqual(r.status,200,data)
        return data

    def run_case(self, env=None):
        self.tmp=tempfile.TemporaryDirectory(prefix='ac-alsa-')
        state=Path(self.tmp.name)/'state';state.mkdir()
        config={'backend':'alsa','device':'hw:0,13','transport':'rtp','playback_confirmed':True}
        (state/'config.json').write_text(json.dumps(config))
        e=dict(os.environ,LD_LIBRARY_PATH=str(LIBDIR));e.update(env or {})
        self.proc=subprocess.Popen([str(BINARY),'--state-dir',str(state)],env=e,
                                   stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        self.addCleanup(self.cleanup)
        for _ in range(100):
            try:
                self.token=json.loads((state/'credentials.json').read_text())['control_token']
                self.api('GET','/v1/status');break
            except (OSError,ValueError,http.client.HTTPException):time.sleep(.02)
        else:self.fail('daemon failed to start')

    def cleanup(self):
        self.proc.terminate();self.proc.wait(timeout=5);self.proc.stderr.close();self.tmp.cleanup()

    def test_partial_frames_xrun_recovery_and_explicit_capabilities(self):
        self.run_case()
        self.api('POST','/v1/start',{})
        time.sleep(.3)
        s=self.api('GET','/v1/status')
        self.assertGreater(s['capture_overruns'],0)
        self.assertEqual(s['signal'],'detected')
        self.assertEqual(s['alsa_pending_ms'],.5)
        self.assertGreater(s['rtp_packets'],10)
        env=dict(os.environ,LD_LIBRARY_PATH=str(LIBDIR))
        p=subprocess.run([str(BINARY),'--probe-device','hw:0,13'],env=env,capture_output=True,check=True)
        caps=json.loads(p.stdout)
        self.assertEqual(caps['formats'],['S16_LE','S32_LE'])
        self.assertEqual(caps['rates_tested_supported'],[48000])
        self.assertEqual(caps['channels_tested_supported'],[2])

    def test_silence_not_signal_or_automatic_provenance_verification(self):
        self.run_case({'FAKE_ALSA_SILENCE':'1'})
        self.api('POST','/v1/config',{'playback_confirmed':False})
        self.api('POST','/v1/test-capture',{})
        time.sleep(.1)
        s=self.api('GET','/v1/status')
        self.assertEqual(s['signal'],'silence')
        self.assertFalse(s['source_verified'])
        self.assertEqual(s['rtp_packets'],0)
        self.assertEqual(s['levels']['rms'],[0,0])

    def test_unavailable_device_backs_off_and_test_expires(self):
        self.run_case({'FAKE_ALSA_UNAVAILABLE':'1'})
        self.api('POST','/v1/test-capture',{})
        time.sleep(.15)
        self.assertEqual(self.api('GET','/v1/status')['state'],'recovering')
        time.sleep(5)
        s=self.api('GET','/v1/status')
        self.assertEqual(s['state'],'idle')
        errors=[x for x in self.api('GET','/v1/logs')['lines'] if 'Capture failure' in x]
        self.assertLessEqual(len(errors),8)
        self.assertIn('unavailable',s['last_error'])

    def test_suspend_recovery_does_not_loop_indefinitely_in_alsa(self):
        self.run_case({'FAKE_ALSA_SUSPEND':'1'})
        self.api('POST','/v1/start',{})
        time.sleep(.15)
        s=self.api('GET','/v1/status')
        self.assertIn('capture suspended',s['last_error'])
        began=time.monotonic()
        self.api('POST','/v1/stop',{})
        self.proc.terminate();self.proc.wait(timeout=2)
        self.assertLess(time.monotonic()-began,1)

    def test_wrong_native_rate_is_rejected_no_silent_resampling(self):
        self.run_case({'FAKE_ALSA_WRONG_RATE':'1'})
        self.api('POST','/v1/start',{})
        time.sleep(.1)
        s=self.api('GET','/v1/status')
        self.assertEqual(s['state'],'recovering')
        self.assertIn('no automatic resampling',s['last_error'])
        self.assertEqual(s['rtp_packets'],0)

if __name__=='__main__':unittest.main()
