# SPDX-License-Identifier: MIT
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import time
import unittest
import wave

PATH=Path(__file__).resolve().parents[1]/'receiver/receiver.py'
spec=importlib.util.spec_from_file_location('ac_receiver',PATH)
r=importlib.util.module_from_spec(spec);sys.modules[spec.name]=r;spec.loader.exec_module(r)

def packet(seq=0,ts=0,ssrc=1,n=480,marker=False):
    return r.Packet(seq,ts,ssrc,marker,struct.pack('!hh',8192,-8192)*n)

class ReceiverTests(unittest.TestCase):
    def test_rtp_endian_header(self):
        raw=struct.pack('!BBHIIhh',0x80,96,65535,0xffffffff,123,0x1234,-32768)
        p=r.parse_rtp(raw,2)
        self.assertEqual(p.seq,65535);self.assertEqual(p.timestamp,0xffffffff)
        self.assertEqual(r.l16_to_le(p.pcm),struct.pack('<hh',0x1234,-32768))

    def test_csrc_extension_padding(self):
        data=struct.pack('!BBHII',0xb1,96,1,2,3)+b'abcd'+b'\xbe\xde\x00\x01'+b'1234'+struct.pack('!hh',1,2)+b'\0\0\0\x04'
        self.assertEqual(r.parse_rtp(data,2).pcm,struct.pack('!hh',1,2))

    def test_malformed_packets(self):
        for raw in [b'',b'\x80'*11,struct.pack('!BBHII',0x40,96,1,2,3)+b'\0'*4,
                    struct.pack('!BBHII',0x81,96,1,2,3),
                    struct.pack('!BBHII',0x90,96,1,2,3)+b'\0'*4,
                    struct.pack('!BBHII',0xa0,96,1,2,3)+b'\0'*4,
                    struct.pack('!BBHII',0x80,97,1,2,3)+b'\0'*4,
                    struct.pack('!BBHII',0x80,96,1,2,3)+b'123']:
            with self.assertRaises(ValueError):r.parse_rtp(raw,2)

    def test_sequence_wrap_duplicate_reorder_and_loss(self):
        s=r.SequenceTracker()
        for n in [65534,0,65535,1]:self.assertTrue(s.put(n))
        self.assertEqual(s.missing,0);self.assertEqual(s.reordered,1)
        self.assertFalse(s.put(0));self.assertEqual(s.duplicates,1)
        s.put(4);self.assertEqual(s.missing,2)
        for n in range(5,10000):s.put(n)
        self.assertLessEqual(len(s.seen),4097)

    def test_timestamp_fragments_reassemble_without_sample_loss(self):
        j=r.JitterBuffer(48000,2,20)
        j.put(packet(1,1000,n=297),1)
        j.put(packet(2,1297,n=183),1.001)
        self.assertIsNone(j.pop(1.019))
        data=j.pop(1.021)
        self.assertEqual(data,struct.pack('<hh',8192,-8192)*480)
        self.assertEqual(j.missing_frames,0)

    def test_loss_produces_aligned_silence_not_stall(self):
        j=r.JitterBuffer(48000,2,0)
        j.put(packet(1,0,n=297),0)
        data=j.pop(.001)
        self.assertEqual(len(data),1920)
        self.assertEqual(data[-183*4:],b'\0'*(183*4))
        self.assertEqual(j.missing_frames,183)
        self.assertEqual(j.pop(.011),b'\0'*1920)
        self.assertIsNone(j.stats()['end_to_end_ms'])

    def test_timestamp_wrap(self):
        j=r.JitterBuffer(48000,2,0)
        j.put(packet(65535,0xffffff00,n=256),0)
        j.put(packet(0,0,n=224),.001)
        self.assertEqual(j.pop(.002),struct.pack('<hh',8192,-8192)*480)

    def test_reordered_packets_and_ssrc_reset(self):
        j=r.JitterBuffer(48000,2,30)
        j.put(packet(0,0,n=240),0)
        j.put(packet(2,480,n=240),.001)
        j.put(packet(1,240,n=240),.002)
        self.assertEqual(j.pop(.031),struct.pack('<hh',8192,-8192)*480)
        j.put(packet(0,123,ssrc=2),.04)
        self.assertEqual(j.ssrc,2);self.assertEqual(len(j.blocks),1)
        self.assertEqual(j.resets,2)

    def test_jitter_buffer_is_bounded_and_resyncs_after_suspension(self):
        j=r.JitterBuffer(48000,2,0)
        for n in range(1000):j.put(packet(n,n*2,n=1),0)
        self.assertLessEqual(len(j.blocks),256)
        self.assertGreater(j.overflow,0)
        self.assertIsNone(j.pop(1));self.assertIsNone(j.cursor)
        j.put(packet(1,100),1.1);self.assertIsNotNone(j.pop(1.101))

    def test_pcm_framing_all_chunk_boundaries(self):
        data=bytes(range(256))*2
        for stride in range(1,33):
            f=r.PCMFramer(4);out=b''
            for off in range(0,len(data),stride):out+=f.feed(data[off:off+stride])
            self.assertEqual(out,data);self.assertEqual(f.pending,b'')

    def test_real_levels_mono_stereo_and_s32(self):
        self.assertEqual(r.levels(struct.pack('<hh',-32768,0),'S16_LE',2),[{'rms':1,'peak':1},{'rms':0,'peak':0}])
        self.assertEqual(r.levels(struct.pack('<i',-2147483648),'S32_LE',1),[{'rms':1,'peak':1}])
        with self.assertRaises(ValueError):r.levels(b'bad','S16_LE',2)

    def test_record_only_when_explicit_and_preserves_alignment(self):
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory)/'diagnostic.wav'
            sink=r.Sink(48000,2,'S16_LE',record=p)
            data=struct.pack('<hh',1,-2)*480
            sink.put(data);sink.close()
            with wave.open(str(p),'rb') as wav:
                self.assertEqual(wav.getparams()[:4],(2,2,48000,480))
                self.assertEqual(wav.readframes(480),data)
            before=p.read_bytes()
            sink=r.Sink(48000,2,'S16_LE',record=p)
            time.sleep(.05)
            with self.assertRaises(RuntimeError):sink.close()
            self.assertEqual(p.read_bytes(),before)

if __name__=='__main__':unittest.main()
