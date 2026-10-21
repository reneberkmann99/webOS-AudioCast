# SPDX-License-Identifier: MIT
"""Optional lighting hook: --hook features:process (from receiver.py).

Computes real PCM RMS/peak and simple causal IIR band-energy/onset proxies.
Not a calibrated FFT, tempo estimator, or a native HyperHDR input protocol.
Replace process() with your LED engine's sample ingestion API if appropriate.
"""
from array import array
import json
import math
import sys
import time


class Features:
    def __init__(self):
        self.low=self.highlow=self.average=self.previous=0.0
        self.last_emit=self.last_beat=0.0
        self.rate=None

    def feed(self, pcm, metadata):
        rate=metadata['rate'];channels=metadata['channels']
        samples=array('h' if metadata['sample_format']=='S16_LE' else 'i');samples.frombytes(pcm)
        if sys.byteorder!='little':samples.byteswap()
        scale=32768.0 if samples.itemsize==2 else 2147483648.0
        if self.rate!=rate:
            self.low=self.highlow=0.0;self.rate=rate
        a=1-math.exp(-2*math.pi*180/rate);b=1-math.exp(-2*math.pi*3500/rate)
        energy=[0.0,0.0,0.0];rms=0.0;peak=0.0;frames=len(samples)//channels
        for pos in range(0,len(samples),channels):
            x=sum(samples[pos:pos+channels])/(channels*scale)
            self.low+=a*(x-self.low);self.highlow+=b*(x-self.highlow)
            bands=(self.low,self.highlow-self.low,x-self.highlow)
            for i,value in enumerate(bands):energy[i]+=value*value
            rms+=x*x;peak=max(peak,abs(x))
        if not frames:return None
        power=rms/frames
        self.average=.98*self.average+.02*power
        now=time.monotonic()
        beat=power>max(.00001,self.average*1.7) and now-self.last_beat>.20
        if beat:self.last_beat=now
        result={'rms':math.sqrt(power),'peak':peak,
                'bass':math.sqrt(energy[0]/frames),'mid':math.sqrt(energy[1]/frames),
                'treble':math.sqrt(energy[2]/frames),
                'transient':max(0.0,math.sqrt(power)-self.previous),'onset':beat,
                'method':'IIR energy proxies, heuristic onset; not calibrated FFT/BPM'}
        self.previous=math.sqrt(power)
        if now-self.last_emit>=.05:
            self.last_emit=now;return result
        return None


_state=Features()
def process(pcm, metadata):
    result=_state.feed(pcm,metadata)
    if result is not None:print(json.dumps({'audio_features':result}),flush=True)
