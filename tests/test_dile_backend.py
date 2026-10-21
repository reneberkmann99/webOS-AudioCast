# SPDX-License-Identifier: MIT
"""Explicit vendor ABI fixture tests. Never evidence of real TV playback or audible reception."""
import http.client
import json
import os
from pathlib import Path
import re
import socket
import struct
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BINARY = ROOT / 'build/native/audiocastd'
LIB = ROOT / 'build/test-lib/libfake_dile.so'

class DileBackendTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        LIB.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(['gcc','-std=c11','-shared','-fPIC','-pthread','-Wall','-Wextra','-Werror',
                        str(ROOT/'tests/fake_dile.c'),'-o',str(LIB)],check=True)

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ac-dile-')
        self.base = Path(self.temp.name)
        self.state = self.base/'state'
        self.trace = self.base/'trace'
        self.env = dict(os.environ,AUDIOCAST_DILE_TEST_LIBRARY=str(LIB),
                        AUDIOCAST_DILE_TEST_LOCK=str(self.base/'upload.lock'),FAKE_DILE_TRACE=str(self.trace))
        self.proc = None
        self.addCleanup(self.cleanup)

    def cleanup(self):
        if self.proc:
            self.proc.terminate()
            try:self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:self.proc.kill();self.proc.wait();raise
            self.proc.stderr.close()
        self.temp.cleanup()

    def events(self):
        return self.trace.read_text().splitlines() if self.trace.exists() else []

    def probe(self, extra=None, env=None, expected=0):
        cmd=[str(BINARY),'--state-dir',str(self.state),'--probe-dile','--seconds','1',
             '--dile-gain-confirmed','--playback-confirmed','--allow-test-source']
        result=subprocess.run(cmd+(extra or []),env=env or self.env,capture_output=True,text=True,timeout=5)
        if expected is not None:self.assertEqual(result.returncode,expected,result.stderr)
        return result

    def test_pcm_delivery_original_gain_restored_and_no_recording(self):
        result=self.probe()
        data=json.loads(re.search(r'DILE_PROBE_RESULT (.+)',result.stdout)[1])
        self.assertGreater(data['captured_frames'],30000)
        self.assertGreater(data['rms'][0],.05)
        self.assertEqual(data['rms'][0],data['rms'][1])
        self.assertTrue(data['gain_restored'])
        self.assertFalse(data['audio_sent']);self.assertFalse(data['audio_saved'])
        self.assertFalse((self.state/'dile-recovery.json').exists())
        events=self.events()
        for item in ['start','register','gain:127','gain:12','unregister','stop']:self.assertIn(item,events)
        self.assertLess(events.index('gain:127'),events.index('gain:12'))
        self.assertLess(events.index('gain:12'),events.index('stop'))
        self.assertEqual(events.count('start'),1)

    def test_consent_and_duration_rejected_before_any_vendor_call(self):
        for args in [[],['--dile-gain-confirmed'],['--playback-confirmed'],
                     ['--dile-gain-confirmed','--playback-confirmed','--seconds','31'],
                     ['--dile-gain-confirmed','--playback-confirmed','--seconds','-1']]:
            result=subprocess.run([str(BINARY),'--state-dir',str(self.state),'--probe-dile']+args,
                                  env=self.env,capture_output=True,text=True,timeout=3)
            self.assertNotEqual(result.returncode,0)
            self.assertEqual(self.events(),[])

    def test_fixture_cannot_bypass_production_firmware_restriction(self):
        result=subprocess.run([str(BINARY),'--state-dir',str(self.state),'--probe-dile','--seconds','1',
                               '--dile-gain-confirmed','--playback-confirmed'],
                              env=self.env,capture_output=True,text=True,timeout=3)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('requires explicit --allow-test-source',result.stderr)
        self.assertEqual(self.events(),[])
        env=dict(os.environ)
        env.pop('AUDIOCAST_DILE_TEST_LIBRARY',None)
        result=subprocess.run([str(BINARY),'--state-dir',str(self.state),'--probe-dile','--seconds','1',
                               '--dile-gain-confirmed','--playback-confirmed'],env=env,capture_output=True,text=True,timeout=3)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('exact inspected',result.stderr)

    def test_partial_gain_failure_restores_original(self):
        result=self.probe(env=dict(self.env,FAKE_DILE_GAIN_FAIL='1'),expected=None)
        self.assertNotEqual(result.returncode,0)
        self.assertEqual(self.events().count('gain:127'),1)
        self.assertEqual(self.events().count('gain:12'),1)
        self.assertIn('stop',self.events())
        self.assertFalse((self.state/'dile-recovery.json').exists())

    def test_failed_start_is_stopped_without_a_gain_write(self):
        self.probe(env=dict(self.env,FAKE_DILE_START_FAIL='1'),expected=1)
        self.assertEqual(self.events(),['start','stop'])

    def test_unexpected_initial_gain_is_not_changed(self):
        self.probe(env=dict(self.env,FAKE_DILE_WRONG_GAIN='1'),expected=1)
        self.assertFalse(any(x.startswith('gain:') for x in self.events()))
        self.assertIn('stop',self.events())

    def test_registration_failure_and_late_callback_cleanup(self):
        self.probe(env=dict(self.env,FAKE_DILE_REGISTER_FAIL='1',FAKE_DILE_LATE_CALLBACK='1'),expected=1)
        self.assertIn('gain:12',self.events())
        self.assertIn('stop',self.events())
        self.assertFalse((self.state/'dile-recovery.json').exists())

    def test_watchdog_expires_without_automatic_restart(self):
        began=time.monotonic()
        self.probe(env=dict(self.env,AUDIOCAST_DILE_TEST_LIMIT_MS='150'),expected=1)
        self.assertLess(time.monotonic()-began,1)
        self.assertEqual(self.events().count('start'),1)
        self.assertEqual(self.events().count('gain:12'),1)
        self.assertIn('stop',self.events())

    def test_failed_restoration_leaves_recovery_record_and_owner_only_rollback(self):
        self.probe(env=dict(self.env,FAKE_DILE_RESTORE_FAIL='1'),expected=1)
        self.assertTrue((self.state/'dile-recovery.json').exists())
        result=subprocess.run([str(BINARY),'--state-dir',str(self.state),'--restore-dile-gain','--allow-test-source'],
                              env=self.env,capture_output=True,text=True,timeout=3)
        self.assertEqual(result.returncode,0,result.stderr)
        self.assertTrue(json.loads(result.stdout)['restored'])
        self.assertFalse((self.state/'dile-recovery.json').exists())

    def api(self,method,path,body=None,expected=200):
        conn=http.client.HTTPConnection('127.0.0.1',7071,timeout=2)
        conn.request(method,path,json.dumps(body) if body is not None else None,
                     {'Authorization':'Bearer '+self.token,'Content-Type':'application/json'})
        response=conn.getresponse();data=json.loads(response.read());conn.close()
        self.assertEqual(response.status,expected,data)
        return data

    def daemon(self,extra_env=None):
        self.state.mkdir(mode=0o700)
        (self.state/'config.json').write_text(json.dumps({'backend':'dile','device':'mstar-upload:0',
          'playback_confirmed':True,'dile_gain_confirmed':True,'autostart':False}))
        self.proc=subprocess.Popen([str(BINARY),'--state-dir',str(self.state),'--allow-test-source'],
            env=dict(self.env,**(extra_env or {})),stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        for _ in range(120):
            try:
                self.token=json.loads((self.state/'credentials.json').read_text())['control_token']
                self.api('GET','/v1/status');return
            except (OSError,ValueError,http.client.HTTPException):
                if self.proc.poll() is not None:self.fail(self.proc.stderr.read().decode())
                time.sleep(.02)
        self.fail('DILE fixture daemon did not start')

    def wait_stopped(self,timeout=3):
        until=time.monotonic()+timeout
        while time.monotonic()<until:
            data=self.api('GET','/v1/status')
            if not data['streaming'] and not data['test_active'] and not data['capture_resources_active']:return data
            time.sleep(.02)
        self.fail('DILE capture failed to stop')

    def test_engine_cap_is_total_fail_closed_and_never_autostarts(self):
        self.daemon({'AUDIOCAST_DILE_TEST_LIMIT_MS':'250'})
        self.assertEqual(self.events(),[],'Loading DILE config must not start hardware')
        self.api('POST','/v1/config',{'autostart':True},expected=400)
        for invalid in [{'rate':44100},{'channels':1},{'sample_format':'S32_LE','transport':'http'},
                        {'dile_gain_confirmed':'yes'},{'device':'hw:0,0'}]:
            self.api('POST','/v1/config',invalid,expected=400)
        self.api('POST','/v1/config',{'dile_gain_confirmed':False})
        self.api('POST','/v1/test-capture',{},expected=400)
        self.api('POST','/v1/config',{'dile_gain_confirmed':True,'playback_confirmed':False})
        self.api('POST','/v1/test-capture',{},expected=400)
        self.api('POST','/v1/config',{'playback_confirmed':True})
        self.api('POST','/v1/test-capture',{})
        time.sleep(.1)
        self.assertGreater(self.api('GET','/v1/status')['levels']['rms'][0],.05)
        stopped=self.wait_stopped()
        self.assertTrue(stopped['dile_gain_restore_verified']);self.assertEqual(stopped['rtp_packets'],0)
        self.assertEqual(stopped['state'],'idle');self.assertEqual(stopped['last_error'],'')
        time.sleep(.4)
        self.assertEqual(self.events().count('start'),1,'No HAL retry/reopening after the capture grant expires')
        self.api('POST','/v1/resume',{},expected=400)

    def test_engine_malformed_pcm_fails_closed_without_retry(self):
        self.daemon({'FAKE_DILE_MALFORMED':'1'})
        self.api('POST','/v1/test-capture',{})
        time.sleep(.2)
        data=self.wait_stopped()
        self.assertEqual(data['state'],'error')
        self.assertTrue(data['dile_gain_restore_verified'])
        self.assertEqual(self.events().count('start'),1)
        time.sleep(.2);self.assertEqual(self.events().count('start'),1)

    def test_exclusive_upload_owner_and_no_rollback_of_live_capture(self):
        self.daemon()
        self.api('POST','/v1/test-capture',{})
        for _ in range(100):
            if (self.state/'dile-recovery.json').exists():break
            time.sleep(.01)
        else:self.fail('missing recovery record during capture')
        result=subprocess.run([str(BINARY),'--state-dir',str(self.state),'--restore-dile-gain','--allow-test-source'],
                              env=self.env,capture_output=True,text=True,timeout=3)
        self.assertNotEqual(result.returncode,0);self.assertIn('still alive',result.stderr)
        other=self.base/'other'
        result=subprocess.run([str(BINARY),'--state-dir',str(other),'--probe-dile','--seconds','1',
           '--dile-gain-confirmed','--playback-confirmed','--allow-test-source'],env=self.env,capture_output=True,text=True,timeout=3)
        self.assertNotEqual(result.returncode,0);self.assertIn('owned by another',result.stderr)
        self.assertEqual(self.events().count('start'),1)
        self.api('POST','/v1/stop',{})
        self.assertTrue(self.wait_stopped()['dile_gain_restore_verified'])

    def test_chunk_queue_preserves_pcm_order_through_real_host_rtp(self):
        self.daemon()
        sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
        sock.bind(('127.0.0.1',0));sock.settimeout(2)
        try:
            self.api('POST','/v1/config',{'destination_port':sock.getsockname()[1]})
            self.api('POST','/v1/start',{})
            packets=[sock.recv(2048) for _ in range(42)]
            samples=[]
            for i,packet in enumerate(packets):
                self.assertEqual(packet[0],0x80);self.assertEqual(packet[1]&127,96)
                data=struct.unpack('!'+'h'*((len(packet)-12)//2),packet[12:])
                self.assertEqual(data[::2],data[1::2]);samples.extend(data[::2])
                if i:
                    old=packets[i-1]
                    self.assertEqual((struct.unpack('!I',packet[4:8])[0]-struct.unpack('!I',old[4:8])[0])&0xffffffff,(len(old)-12)//4)
            for previous,current in zip(samples,samples[1:]):
                self.assertEqual(current,((previous//64+101)%200-100)*64)
            self.api('POST','/v1/stop',{})
            stopped=self.wait_stopped()
            self.assertTrue(stopped['dile_gain_restore_verified'])
            self.assertEqual(stopped['backend_capture']['dropped_bytes'],0)
        finally:sock.close()

    def test_chunk_queue_drops_bounded_backlog_without_blocking_callback(self):
        self.daemon({'FAKE_DILE_BURST':'1'})
        self.api('POST','/v1/test-capture',{})
        time.sleep(.2)
        self.api('POST','/v1/stop',{})
        stopped=self.wait_stopped()
        self.assertTrue(stopped['dile_gain_restore_verified'])
        self.assertGreater(stopped['backend_capture']['dropped_bytes'],0)

if __name__=='__main__':unittest.main()
