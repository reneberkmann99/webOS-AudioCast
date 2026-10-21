#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host-only synthetic streaming benchmark. Never evidence of TV/audio-point latency."""
import argparse
import http.client
import json
import os
from pathlib import Path
import select
import socket
import statistics
import struct
import subprocess
import tempfile
import time

ROOT=Path(__file__).resolve().parents[1]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--seconds-per-profile',type=float,default=30)
    p.add_argument('--output',type=Path,default=ROOT/'build/proofs/host-performance.json')
    args=p.parse_args()
    if args.seconds_per_profile<=0:p.error('positive duration required')
    results=[]
    with tempfile.TemporaryDirectory(prefix='ac-soak-') as temp:
        state=Path(temp)/'state'
        proc=subprocess.Popen([str(ROOT/'build/native/audiocastd'),'--state-dir',str(state),'--allow-test-source'],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        try:
            for _ in range(200):
                try:token=json.loads((state/'credentials.json').read_text())['control_token'];break
                except (FileNotFoundError,ValueError):time.sleep(.02)
            def api(method,path,body=None):
                c=http.client.HTTPConnection('127.0.0.1',7071,timeout=3)
                c.request(method,path,None if body is None else json.dumps(body),{'Authorization':'Bearer '+token})
                r=c.getresponse();data=json.loads(r.read());c.close()
                if r.status!=200:raise RuntimeError(data)
                return data
            for _ in range(100):
                try:api('GET','/v1/status');break
                except OSError:time.sleep(.02)
            for ms,profile in [(5,'low-latency'),(10,'low-latency'),(20,'low-latency'),(20,'stable')]:
                api('POST','/v1/stop',{});time.sleep(.06)
                udp=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);udp.bind(('127.0.0.1',0));udp.setblocking(False)
                api('POST','/v1/config',{'backend':'test-tone','device':'','playback_confirmed':True,
                     'transport':'rtp','packet_ms':ms,'profile':profile,'destination':'127.0.0.1',
                     'destination_port':udp.getsockname()[1]})
                before=api('GET','/v1/status');api('POST','/v1/start',{})
                start=time.monotonic();next_sample=start+1;packets=bytes_=gap=max_size=0;last=None;samples=[]
                while time.monotonic()-start<args.seconds_per_profile:
                    ready,_,_=select.select([udp],[],[],.02)
                    if ready:
                        for _ in range(64):
                            try:data=udp.recv(2048)
                            except BlockingIOError:break
                            packets+=1;bytes_+=len(data);max_size=max(max_size,len(data))
                            seq=struct.unpack('!H',data[2:4])[0]
                            if last is not None:
                                distance=(seq-last)&65535
                                if 1<distance<32768:gap+=distance-1
                            last=seq
                    if time.monotonic()>=next_sample:
                        samples.append(api('GET','/v1/status'));next_sample+=1
                elapsed=time.monotonic()-start
                final=api('GET','/v1/status');api('POST','/v1/stop',{});udp.close()
                ages=[s['capture_to_network_ms'] for s in samples if s['capture_to_network_ms'] is not None]
                result={'capture_block_ms':ms,'profile':profile,'elapsed_seconds':elapsed,
                        'packets_received':packets,'udp_sequence_gaps_observed':gap,
                        'max_rtp_datagram_bytes':max_size,'observed_mbit_s':bytes_*8/elapsed/1e6,
                        'status_samples':len(samples),'cpu_percent_mean':statistics.mean(s['cpu_percent'] for s in samples) if samples else None,
                        'cpu_percent_peak':max((s['cpu_percent'] for s in samples),default=None),
                        'rss_kb_peak':max((s['rss_kb'] for s in samples),default=None),
                        'capture_read_to_sender_ms_min':min(ages,default=None),'capture_read_to_sender_ms_max':max(ages,default=None),
                        'capture_queue_drops':final['queue_drops']-before['queue_drops'],
                        'rtp_send_errors':final['rtp_errors']-before['rtp_errors']}
                results.append(result);print(json.dumps(result),flush=True)
        finally:
            proc.terminate();proc.wait(timeout=5);proc.stderr.close()
    output={'source':'generated host test tone; NOT TV playback','host_machine':os.uname().machine,
            'hardware_qualified':False,'end_to_end_tv_latency_ms':None,
            'alsa_buffer_performance_measured':False,'profiles':results}
    args.output.parent.mkdir(parents=True,exist_ok=True);args.output.write_text(json.dumps(output,indent=2)+'\n')

if __name__=='__main__':main()
