# SPDX-License-Identifier: MIT
"""Actual host supervisor lifecycle in isolated directories, NOT webOS/HBC tests."""
import concurrent.futures
import http.client
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT=Path(__file__).resolve().parents[1]

class ServiceTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='ac-service-')
        self.base=Path(self.tmp.name);self.app=self.base/'app';self.state=self.base/'state';self.hooks=self.base/'hooks'
        (self.app/'bin').mkdir(parents=True);self.hooks.mkdir()
        shutil.copytree(ROOT/'service',self.app/'service')
        (self.app/'bin/audiocastd').symlink_to(ROOT/'build/native/audiocastd')
        # Only the root gate is doubled. No real system paths/services are modified.
        mocks=self.base/'mocks';mocks.mkdir();identity=mocks/'id';identity.write_text('#!/bin/sh\nif [ "$1" = -u ]; then echo 0; else exec /usr/bin/id "$@"; fi\n');identity.chmod(0o755)
        self.env=dict(os.environ,AUDIOCAST_APP_DIR=str(self.app),AUDIOCAST_STATE_DIR=str(self.state),
                      AUDIOCAST_HOOK_DIR=str(self.hooks),PATH=str(mocks)+':'+os.environ['PATH'])
        self.manage=self.app/'service/startup/manage.sh'
        self.creds=None

    def run_manage(self,action,check=True):
        return subprocess.run(['sh',str(self.manage),action],env=self.env,capture_output=True,text=True,timeout=10,check=check)

    def ensure(self):
        self.creds=json.loads(self.run_manage('ensure').stdout)
        for _ in range(100):
            try:return self.api('GET','/v1/status')
            except (OSError,http.client.HTTPException):time.sleep(.05)
        self.fail('supervised native daemon not available')

    def api(self,method,path,body=None):
        c=http.client.HTTPConnection('127.0.0.1',7071,timeout=2)
        c.request(method,path,json.dumps(body) if body is not None else None,
                  {'Authorization':'Bearer '+self.creds['control_token']})
        r=c.getresponse();out=json.loads(r.read());c.close();self.assertEqual(r.status,200,out);return out

    def tearDown(self):
        try:self.run_manage('stop',check=False)
        except (OSError,subprocess.TimeoutExpired):pass
        deadline=time.monotonic()+8
        while time.monotonic()<deadline:
            p=self.state/'supervisor.pid'
            if not p.exists():break
            time.sleep(.05)
        self.tmp.cleanup()

    def test_ensure_detaches_and_duplicate_instances_are_prevented(self):
        self.ensure()
        pid=(self.state/'daemon.pid').read_text()
        supervisor=(self.state/'supervisor.pid').read_text()
        again=json.loads(self.run_manage('ensure').stdout)
        self.assertEqual(again,self.creds)
        self.assertEqual((self.state/'daemon.pid').read_text(),pid)
        self.assertEqual((self.state/'supervisor.pid').read_text(),supervisor)
        self.assertEqual(self.api('GET','/v1/status')['state'],'idle')
        self.assertEqual((self.state/'credentials.json').stat().st_mode & 0o777,0o600)

    def test_boot_hook_is_symlink_and_foreign_files_are_preserved(self):
        self.ensure()
        self.api('POST','/v1/config',{'device':'hw:0,13','playback_confirmed':True,'autostart':True})
        self.run_manage('enable');hook=self.hooks/'audiocast'
        self.assertTrue(hook.is_symlink());self.assertEqual(hook.resolve(),self.app/'service/startup/boot-hook.sh')
        self.run_manage('disable');self.assertFalse(hook.exists())
        hook.write_text('foreign file')
        self.assertNotEqual(self.run_manage('enable',False).returncode,0)
        self.assertNotEqual(self.run_manage('disable',False).returncode,0)
        self.assertEqual(hook.read_text(),'foreign file')

    def test_uninstall_removing_binary_stops_daemon_and_supervisor(self):
        self.ensure();pid=int((self.state/'daemon.pid').read_text())
        (self.app/'bin/audiocastd').unlink()
        for _ in range(120):
            if not Path('/proc/'+str(pid)).exists() and not (self.state/'supervisor.pid').exists():break
            time.sleep(.05)
        else:self.fail('uninstall did not stop detached processes')

    def test_service_boot_enable_is_independent_of_stream_autostart(self):
        self.ensure();self.assertFalse(self.api('GET','/v1/config')['autostart'])
        self.run_manage('enable')
        self.assertTrue((self.hooks/'audiocast').is_symlink())
        self.assertEqual(self.api('GET','/v1/status')['state'],'idle')

    def test_boot_hook_initializes_cold_state_without_starting_capture(self):
        subprocess.run(['sh',str(self.app/'service/startup/boot-hook.sh')],env=self.env,
                       capture_output=True,timeout=5,check=True)
        for _ in range(100):
            if (self.state/'credentials.json').exists():break
            time.sleep(.05)
        self.creds=json.loads((self.state/'credentials.json').read_text())
        for _ in range(100):
            try:status=self.api('GET','/v1/status');break
            except (OSError,http.client.HTTPException):time.sleep(.05)
        else:self.fail('boot hook did not start daemon')
        self.assertEqual(status['state'],'idle');self.assertFalse(status['streaming'])
        self.assertEqual(self.state.stat().st_mode & 0o777,0o700)

    def test_unknown_management_action_rejected(self):
        result=self.run_manage('anything; echo unsafe',False)
        self.assertEqual(result.returncode,2)

    def recovery_fixture(self, fail=False):
        binary=self.app/'bin/audiocastd';binary.unlink()
        binary.write_text('#!/bin/bash\nfor arg in "$@"; do\n'
          ' if [ "$arg" = --restore-dile-gain ]; then\n'
          '  echo attempt >> "$AUDIOCAST_STATE_DIR/recovery-attempts"\n' +
          ('  exit 1\n' if fail else '  rm -f "$AUDIOCAST_STATE_DIR/dile-recovery.json"\n  echo restored\n  exit 0\n') +
          ' fi\ndone\nexec -a "$0" "'+str(ROOT/'build/native/audiocastd')+'" "$@"\n')
        binary.chmod(0o755)
        self.state.mkdir(mode=0o700)
        (self.state/'dile-recovery.json').write_text('{}')  # Only the shell dispatch is doubled.

    def test_interrupted_dile_transaction_recovered_before_daemon_launch(self):
        self.recovery_fixture()
        self.ensure()
        self.assertFalse((self.state/'dile-recovery.json').exists())
        self.assertEqual((self.state/'recovery-attempts').read_text().splitlines(),['attempt'])
        self.assertEqual(self.api('GET','/v1/status')['state'],'idle')
        self.assertFalse(self.api('GET','/v1/status')['streaming'])

    def test_failed_dile_recovery_quarantines_without_repeated_writes(self):
        self.recovery_fixture(fail=True)
        self.run_manage('ensure')
        for _ in range(100):
            if (self.state/'quarantined').exists() and not (self.state/'supervisor.pid').exists():break
            time.sleep(.05)
        else:self.fail('failed DILE recovery did not quarantine service')
        self.assertTrue((self.state/'dile-recovery.json').exists())
        self.assertFalse((self.state/'daemon.pid').exists())
        self.assertEqual((self.state/'recovery-attempts').read_text().splitlines(),['attempt'])

    def test_legacy_busybox_timeout_is_used_with_t_option(self):
        mocks=self.base/'mocks'
        timeout=mocks/'timeout'
        timeout.write_text('#!/bin/sh\nif [ "$1" = --help ]; then echo "Usage: timeout [-t SECS] [-s SIG] PROG ARGS"; exit 0; fi\n[ "$1" = -t ] || exit 91\nshift\nexec /usr/bin/timeout "$@"\n')
        timeout.chmod(0o755)
        luna=mocks/'luna-send'
        luna.write_text('#!/bin/sh\necho \'{"returnValue":true,"soundOut":"test-double"}\'\n')
        luna.chmod(0o755)
        self.ensure()
        for _ in range(100):
            platform=self.state/'platform.json'
            if platform.exists() and platform.stat().st_size:break
            time.sleep(.05)
        else:self.fail('legacy BusyBox timeout prevented telemetry command execution')
        self.assertEqual(json.loads(platform.read_text())['soundOut'],'test-double')

if __name__=='__main__':unittest.main()
