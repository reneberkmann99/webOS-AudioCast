#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""webOS AudioCast reference receiver. No audio is saved unless --record is set.

Stdlib networking, bounded RTP jitter buffer, optional sounddevice playback.
A processing hook receives interleaved little-endian PCM bytes + format metadata.
RTP timestamps alone CANNOT measure TV-to-receiver latency.
"""
from __future__ import annotations
import argparse
from array import array
from dataclasses import dataclass
import importlib
import json
import math
from pathlib import Path
import queue
import signal
import socket
import struct
import sys
import threading
import time
import urllib.error
import urllib.request
import wave


def delta(value: int, base: int, bits: int) -> int:
    """Signed modular distance, valid within half the sequence space."""
    return ((value - base + (1 << (bits-1))) % (1 << bits)) - (1 << (bits-1))


@dataclass(frozen=True)
class Packet:
    seq: int
    timestamp: int
    ssrc: int
    marker: bool
    pcm: bytes                 # Signed L16 NETWORK-order samples


def parse_rtp(data: bytes, channels: int, payload_type: int = 96) -> Packet:
    if len(data) < 12 or data[0] >> 6 != 2:
        raise ValueError('invalid RTP v2 header')
    if data[1] & 127 != payload_type:
        raise ValueError('unexpected RTP payload type')
    offset = 12 + 4 * (data[0] & 15)
    if offset > len(data):
        raise ValueError('truncated CSRC list')
    if data[0] & 16:
        if offset + 4 > len(data):
            raise ValueError('truncated RTP extension')
        words = struct.unpack_from('!H', data, offset + 2)[0]
        offset += 4 + 4*words
        if offset > len(data):
            raise ValueError('truncated RTP extension data')
    end = len(data)
    if data[0] & 32:
        pad = data[-1]
        if not pad or pad > end-offset:
            raise ValueError('invalid RTP padding')
        end -= pad
    payload = data[offset:end]
    if channels not in (1, 2) or not payload or len(payload) % (2*channels):
        raise ValueError('unaligned L16 payload')
    seq, stamp, ssrc = struct.unpack_from('!HII', data, 2)
    return Packet(seq, stamp, ssrc, bool(data[1] & 128), payload)


def l16_to_le(data: bytes) -> bytes:
    if len(data) % 2:
        raise ValueError('unaligned L16 data')
    samples = array('h')
    samples.frombytes(data)
    # Input network big-endian, output little-endian regardless of host endianness.
    samples.byteswap()
    return samples.tobytes()


class SequenceTracker:
    def __init__(self):
        self.highest = None
        self.seen = set()
        self.received = self.duplicates = self.reordered = self.gaps = self.recovered = 0

    def put(self, seq: int) -> bool:
        extended = seq if self.highest is None else self.highest + delta(seq, self.highest & 65535, 16)
        if self.highest is not None and extended < self.highest - 4096:
            self.reordered += 1
            return False
        if extended in self.seen:
            self.duplicates += 1
            return False
        if self.highest is not None:
            if extended < self.highest:
                self.reordered += 1
                self.recovered += 1
            elif extended > self.highest+1:
                self.gaps += extended-self.highest-1
        self.highest = extended if self.highest is None else max(self.highest, extended)
        self.seen.add(extended)
        self.seen = {v for v in self.seen if v >= self.highest-4096}
        self.received += 1
        return True

    @property
    def missing(self):
        # Gaps can be repaired by later out-of-order packets; playout loss is separate.
        return max(0, self.gaps-self.recovered)


class JitterBuffer:
    """Timestamp-indexed bounded L16 buffer, one ingest/playout owner.

    Startup waits jitter_ms. Afterwards playout runs every10ms even on packet loss.
    Missing frames become silence, old packets are discarded, huge jumps resync.
    """
    def __init__(self, rate: int, channels: int, jitter_ms: int = 30):
        self.rate, self.channels, self.jitter_ms = rate, channels, jitter_ms
        self.align = channels*2
        self.blocks: dict[int, bytes] = {}
        self.cursor = None
        self.deadline = None
        self.ssrc = None
        self.tracker = SequenceTracker()
        self.last_seq = None
        self.late = self.overflow = self.underruns = self.missing_frames = self.resets = 0
        self.jitter_seconds = 0.0
        self.previous_transit = None
        self.max_packets = 256
        self.max_frames = rate // 2
        self.last_arrival = None

    def reset(self, packet: Packet, now: float):
        self.blocks.clear()
        self.cursor = packet.timestamp
        self.deadline = now + self.jitter_ms/1000
        self.ssrc = packet.ssrc
        self.tracker = SequenceTracker()
        self.previous_transit = None
        self.resets += 1

    def put(self, packet: Packet, now: float):
        if self.ssrc != packet.ssrc or self.cursor is None:
            self.reset(packet, now)
        stamp = self.cursor + delta(packet.timestamp, self.cursor & 0xffffffff, 32)
        # A discontinuity marker from the sender establishes a new playout origin.
        # Don't repeatedly reset for reordered older marker packets.
        forward = self.tracker.highest is None or delta(packet.seq, self.tracker.highest & 65535,16)>0
        if forward and ((packet.marker and self.tracker.highest is not None) or abs(stamp-self.cursor)>self.rate):
            self.reset(packet, now)
            stamp = packet.timestamp
        if not self.tracker.put(packet.seq):
            return
        self.last_arrival = now
        n = len(packet.pcm)//self.align
        if stamp+n <= self.cursor:
            self.late += 1
            return
        transit = now - stamp/self.rate
        if self.previous_transit is not None:
            self.jitter_seconds += (abs(transit-self.previous_transit)-self.jitter_seconds)/16
        self.previous_transit = transit
        if stamp-self.cursor > self.max_frames or len(self.blocks) >= self.max_packets:
            self.overflow += 1
            return
        self.blocks[stamp] = packet.pcm

    def pop(self, now: float) -> bytes | None:
        if self.deadline is None or now < self.deadline:
            return None
        # Don't replay unbounded stale backlog if the consumer was suspended.
        if now-self.deadline > .25:
            self.blocks.clear(); self.deadline=None; self.cursor=None; self.ssrc=None
            self.overflow += 1
            return None
        frames = self.rate//100
        end = self.cursor+frames
        out = bytearray(frames*self.align)
        covered = bytearray(frames)
        for stamp in sorted(list(self.blocks)):
            data = self.blocks[stamp]
            last = stamp+len(data)//self.align
            if last <= self.cursor:
                del self.blocks[stamp]
                continue
            if stamp >= end:
                break
            lo, hi = max(stamp,self.cursor), min(last,end)
            out[(lo-self.cursor)*self.align:(hi-self.cursor)*self.align] = data[(lo-stamp)*self.align:(hi-stamp)*self.align]
            covered[lo-self.cursor:hi-self.cursor] = b'\x01'*(hi-lo)
            if last <= end:
                del self.blocks[stamp]
            else:
                self.blocks[end] = data[(end-stamp)*self.align:]
                del self.blocks[stamp]
        missing = frames-sum(covered)
        if missing:
            self.underruns += 1
            self.missing_frames += missing
        self.cursor = end
        self.deadline += frames/self.rate
        return l16_to_le(bytes(out))

    def stats(self):
        return {'packets':self.tracker.received,'packet_gaps_unrepaired':self.tracker.missing,
                'duplicates':self.tracker.duplicates,'reordered':self.tracker.reordered,
                'late_packets':self.late,'buffer_overflows':self.overflow,
                'playout_underruns':self.underruns,'missing_frames':self.missing_frames,
                'interarrival_jitter_ms':self.jitter_seconds*1000,
                'receiver_jitter_target_ms':self.jitter_ms,'end_to_end_ms':None}


class PCMFramer:
    def __init__(self, alignment):
        self.alignment = alignment
        self.pending = bytearray()

    def feed(self, data):
        self.pending.extend(data)
        n = len(self.pending)//self.alignment*self.alignment
        out=bytes(self.pending[:n]); del self.pending[:n]
        return out


def levels(pcm: bytes, sample_format: str, channels: int):
    width = 2 if sample_format=='S16_LE' else 4
    if len(pcm) % (width*channels):
        raise ValueError('unaligned PCM')
    samples=array('h' if width==2 else 'i');samples.frombytes(pcm)
    if sys.byteorder != 'little':samples.byteswap()
    scale=float(1 << (width*8-1))
    output=[]
    for ch in range(channels):
        data=samples[ch::channels]
        output.append({'rms':math.sqrt(sum((v/scale)**2 for v in data)/len(data)) if data else 0,
                       'peak':max((abs(v/scale) for v in data),default=0)})
    return output


class Sink:
    """Bounded worker isolates networking from slow playback/hooks/explicit disk recording."""
    def __init__(self, rate, channels, sample_format, play=False, record=None, hook=None, device=None):
        self.format={'rate':rate,'channels':channels,'sample_format':sample_format}
        self.align=channels*(2 if sample_format=='S16_LE' else 4)
        self.queue=queue.Queue(maxsize=8)
        self.drops=0;self.playback_underruns=0;self.last_levels=[];self.error=None
        self.finished=threading.Event();self.record=record;self.play=play;self.hook=hook;self.device=device
        self.worker=threading.Thread(target=self.run,name='audiocast-sink',daemon=True)
        self.worker.start()

    def put(self, pcm):
        # Split large HTTP reads to keep the queue bounded in BOTH item count and bytes.
        max_bytes=self.format['rate']//100*self.align
        for off in range(0,len(pcm),max_bytes):
            block=pcm[off:off+max_bytes]
            if len(block)%self.align:raise ValueError('unaligned sink input')
            try:self.queue.put_nowait(block)
            except queue.Full:
                try:self.queue.get_nowait()
                except queue.Empty:pass
                self.drops+=1
                try:self.queue.put_nowait(block)
                except queue.Full:self.drops+=1

    def run(self):
        playback=None;recording=None
        try:
            if self.play:
                try:import sounddevice
                except ImportError as exc:raise RuntimeError('Optional playback needs pip install -r receiver/requirements.txt') from exc
                playback=sounddevice.RawOutputStream(samplerate=self.format['rate'],channels=self.format['channels'],
                     dtype='int16' if self.format['sample_format']=='S16_LE' else 'int32',latency='low',
                     device=self.device)
                playback.start()
            if self.record:
                # Exclusive creation prevents accidentally overwriting an existing recording.
                self.record_file=open(self.record,'xb')
                recording=wave.open(self.record_file,'wb')
                recording.setnchannels(self.format['channels']);recording.setsampwidth(self.align//self.format['channels']);recording.setframerate(self.format['rate'])
            while not self.finished.is_set() or not self.queue.empty():
                try:data=self.queue.get(timeout=.1)
                except queue.Empty:continue
                self.last_levels=levels(data,self.format['sample_format'],self.format['channels'])
                if recording:recording.writeframesraw(data)
                if self.hook:self.hook(data,dict(self.format))
                if playback and playback.write(data):self.playback_underruns+=1
        except Exception as exc:self.error=str(exc)
        finally:
            if recording:recording.close();self.record_file.close()
            if playback:playback.stop();playback.close()

    def close(self):
        self.finished.set();self.worker.join(timeout=3)
        if self.worker.is_alive():raise RuntimeError('playback/processing worker did not stop; hook must not block')
        if self.error:raise RuntimeError(self.error)


def load_hook(name):
    if not name:return None
    module,separator,function=name.partition(':')
    if not separator:raise ValueError('--hook must be local.module:function (trusted local Python code)')
    hook=getattr(importlib.import_module(module),function)
    if not callable(hook):raise ValueError('hook must be callable')
    return hook


def receiver(args, stop):
    deadline=time.monotonic()+args.duration if args.duration else float('inf')
    sink=None;total=0;last_print=0;jitter=None;sock=None;last_peer=None
    http_queue=queue.Queue(maxsize=8);http_done=threading.Event();http_errors=[]
    def report():
        nonlocal last_print
        now=time.monotonic()
        if now-last_print<1:return
        data=jitter.stats() if jitter else {'transport':'HTTP','end_to_end_ms':None}
        data.update({'received_pcm_bytes':total,'levels':sink.last_levels,'sink_drops':sink.drops,
                     'playback_underruns':sink.playback_underruns,'http_queue_drops':http_drops[0]})
        print(json.dumps(data),flush=True);last_print=now
    http_drops=[0]
    try:
        if args.mode=='rtp':
            sink=Sink(args.rate,args.channels,'S16_LE',args.play,args.record,load_hook(args.hook),args.device)
            jitter=JitterBuffer(args.rate,args.channels,args.jitter_ms)
            family=socket.AF_INET6 if ':' in args.bind else socket.AF_INET
            sock=socket.socket(family,socket.SOCK_DGRAM);sock.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,65536)
            sock.bind((args.bind,args.port));sock.setblocking(False)
            while not stop.is_set() and time.monotonic()<deadline:
                for _ in range(64):
                    try:data,peer=sock.recvfrom(65536)
                    except BlockingIOError:break
                    if args.source and peer[0]!=args.source:continue
                    # Pin first sender address. RTP itself is not authenticated; use a trusted LAN.
                    if last_peer is not None and peer[0]!=last_peer[0]:continue
                    try:packet=parse_rtp(data,args.channels,args.payload_type)
                    except ValueError:continue
                    last_peer=peer;jitter.put(packet,time.monotonic())
                for _ in range(25):
                    pcm=jitter.pop(time.monotonic())
                    if pcm is None:break
                    total+=len(pcm);sink.put(pcm)
                if sink.error:raise RuntimeError(sink.error)
                report();stop.wait(.002)
        else:
            from urllib.parse import urlsplit
            parsed=urlsplit(args.url)
            if parsed.scheme not in ('http','https') or parsed.username or parsed.password or parsed.query or parsed.fragment:
                raise ValueError('use plain HTTP/HTTPS URL without embedded credentials/query/fragment')
            token=Path(args.token_file).read_text().strip()
            if not token or '\r' in token or '\n' in token:raise ValueError('invalid token file')
            request=urllib.request.Request(args.url,headers={'Authorization':'Bearer '+token})
            # Do not send stream tokens through ambient HTTP proxies or cross-host redirects.
            class NoRedirect(urllib.request.HTTPRedirectHandler):
                def redirect_request(self,*unused,**kwargs):raise ValueError('HTTP redirects rejected to protect bearer token')
            opener=urllib.request.build_opener(urllib.request.ProxyHandler({}),NoRedirect)
            response=opener.open(request,timeout=3)
            rate=int(response.headers['X-Audio-Rate']);channels=int(response.headers['X-Audio-Channels']);fmt=response.headers['X-Audio-Format']
            if rate not in (32000,44100,48000,96000) or channels not in (1,2) or fmt not in ('S16_LE','S32_LE'):
                raise ValueError('unsupported HTTP PCM metadata')
            if response.headers.get_content_type()!='application/octet-stream':raise ValueError('unexpected HTTP stream format')
            sink=Sink(rate,channels,fmt,args.play,args.record,load_hook(args.hook),args.device)
            framer=PCMFramer(channels*(2 if fmt=='S16_LE' else 4))
            def http_read():
                try:
                    while not stop.is_set():
                        data=response.read1(4096)
                        if not data:break
                        try:http_queue.put_nowait(data)
                        except queue.Full:
                            # Do not discard arbitrary HTTP bytes: that could misalign samples.
                            # Fail visibly rather than silently corrupting PCM framing.
                            raise RuntimeError('HTTP consumer too slow; reconnect instead of misaligning samples')
                except Exception as exc:
                    if not stop.is_set():http_errors.append(str(exc))
                finally:http_done.set()
            worker=threading.Thread(target=http_read,name='audiocast-http',daemon=True);worker.start()
            try:
                while not stop.is_set() and time.monotonic()<deadline:
                    try:data=http_queue.get(timeout=.05)
                    except queue.Empty:
                        if http_done.is_set():break
                        report();continue
                    pcm=framer.feed(data);total+=len(pcm);sink.put(pcm)
                    if sink.error:raise RuntimeError(sink.error)
                    report()
                if http_errors:raise RuntimeError(http_errors[0])
                if http_done.is_set() and framer.pending:raise ValueError('HTTP ended with incomplete sample frame')
            finally:
                stop.set();response.close();worker.join(timeout=4)
        return {'received_pcm_bytes':total,**(jitter.stats() if jitter else {})}
    finally:
        if sock:sock.close()
        if sink:sink.close()


def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    common=argparse.ArgumentParser(add_help=False)
    common.add_argument('--play',action='store_true',help='optional sounddevice/PortAudio playback')
    common.add_argument('--device',help='output device name/index for --play (e.g. a BlackHole loopback or AirPods); default = system default output')
    common.add_argument('--record',type=Path,help='explicit WAV recording; never default')
    common.add_argument('--duration',type=float,default=0,help='stop after this many seconds')
    common.add_argument('--hook',help='trusted local Python module:function PCM processing callback')
    modes=parser.add_subparsers(dest='mode',required=True)
    rtp=modes.add_parser('rtp',parents=[common]);rtp.add_argument('--bind',default='0.0.0.0');rtp.add_argument('--port',type=int,default=5004)
    rtp.add_argument('--rate',type=int,default=48000,choices=[32000,44100,48000,96000]);rtp.add_argument('--channels',type=int,default=2,choices=[1,2]);rtp.add_argument('--payload-type',type=int,default=96)
    rtp.add_argument('--jitter-ms',type=int,default=30,choices=range(0,201));rtp.add_argument('--source',help='accept only this numeric TV source IP')
    http=modes.add_parser('http',parents=[common]);http.add_argument('--url',required=True);http.add_argument('--token-file',required=True)
    args=parser.parse_args(argv)
    if args.duration<0 or args.record and not (0<args.duration<=3600):parser.error('recording requires explicit --duration in (0,3600] seconds')
    if args.mode=='rtp' and not (1024<=args.port<=65535 and 0<=args.payload_type<=127):parser.error('invalid UDP port/payload type')
    stop=threading.Event()
    signal.signal(signal.SIGINT,lambda *_:stop.set());signal.signal(signal.SIGTERM,lambda *_:stop.set())
    try:result=receiver(args,stop)
    except (OSError,ValueError,RuntimeError,urllib.error.URLError) as exc:
        print('AudioCast receiver:',exc,file=sys.stderr);return 1
    print(json.dumps({'final':result}),flush=True);return 0

if __name__=='__main__':
    raise SystemExit(main())
