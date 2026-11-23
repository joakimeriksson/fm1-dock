#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The DSP of the effects DRIVE, CRUSH, PHASER and TREMOLO on this computer: each effect's own source is compiled
with tests/fx_host.c, which drives its set() and run() in blocks of 256 frames, and sine waves, chords and noise go
through it. What is checked: nothing leaves +-32767; MIX 0 is the input bit for bit; silence stays silent (no DC, no
hiss); DRIVE's harmonics grow with DRIVE and TONE darkens it; CRUSH has no more levels than its bits and holds RATE
frames; PHASER's spectrum moves, left and right differ, and a feedback of 100 % stays bounded and settles; TREMOLO
swings the level. It says nothing about how it sounds, or about the time it takes (tests/emu_test.py and fm1-live do).
  uv run --with numpy tests/fx_dsp_test.py     (a C compiler as `cc`)"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

SRC = Path(__file__).resolve().parents[1]
TMP = tempfile.mkdtemp(prefix="fxdsp")
os.chdir(TMP)
for name, d in (("drive", "fxdrive"), ("crush", "fxcrush"), ("phaser", "fxphaser"), ("tremolo", "fxtremolo")):
    r = subprocess.run(["cc", "-O1", "-Wall", "-Wno-unknown-attributes", f'-DFXSRC="{SRC}/modules/{d}/{name}.c"',
                        f"-I{SRC}/firmware/dock", str(SRC / "tests/fx_host.c"), "-o", f"h_{name}", "-lm"], capture_output=True, text=True)
    if r.returncode:
        sys.exit(r.stderr)
FS=44100; N=FS*2
rng=np.random.default_rng(1)
def sine(a,f=440,n=N): return a*np.sin(2*np.pi*f*np.arange(n)/FS)
def st(x,y=None): y=x if y is None else y; return np.stack([x,y],1).astype(np.int16)
def run(name,x,*p):
    x.tofile('in.raw'); r=subprocess.run(['./h_'+name,'in.raw','out.raw',*map(str,p)],capture_output=True,text=True)
    if r.returncode: return None,r.stderr.strip()
    return np.fromfile('out.raw',dtype=np.int16).reshape(-1,2).astype(float),''
def rms(x): return float(np.sqrt(np.mean(x.astype(float)**2)))
def thd(y,f=440):
    y=y[FS//2:,0]; w=np.hanning(len(y)); S=np.abs(np.fft.rfft(y*w)); fr=np.fft.rfftfreq(len(y),1/FS)
    def band(c): m=(abs(fr-c)<30); return S[m].max()
    f1=band(f); h=np.sqrt(sum(band(f*k)**2 for k in range(2,12) if f*k<20000)); return h/f1
ok=True
def chk(c,msg):
    global ok; print(('  ok   ' if c else '  FAIL ')+msg); ok&=bool(c)
chord=sum(sine(4200,f) for f in (65,130,196,262,330,392,494,659))
loud=st(chord); quiet=st(sine(1000)); mid=st(sine(12000)); sil=st(np.zeros(N)); noise=st(rng.normal(0,6000,N).clip(-32000,32000),rng.normal(0,6000,N).clip(-32000,32000))
print("DRIVE")
y,e=run('drive',mid,0,100,100,100); chk(y is not None,'low drive, full tone/level/mix runs')
prev=-1
for d in (0,20,40,60,80,100):
    y,e=run('drive',mid,d,100,100,100); t=thd(y); chk(t>=prev-1e-3,f'drive {d:3d}: harmonics/fund {t:.3f}, peak {abs(y).max():.0f}, rms {rms(y):.0f}'); prev=t
y,e=run('drive',loud,100,60,100,100); chk(y is not None and abs(y).max()<=32767,f'loud chord, drive 100: peak {abs(y).max():.0f} (in {abs(loud).max()})')
y,e=run('drive',mid,60,60,100,0); chk(np.array_equal(y,mid.astype(float)),'mix 0 = the clean sound, bit for bit')
y,e=run('drive',mid,60,60,0,100); chk(abs(y[2000:]).max()<=1,'level 0 = silence')
y,e=run('drive',mid,100,60,70,100); chk(abs(y[FS//2:].mean())<150,f'no DC offset ({y[FS//2:].mean():.1f})')
y,e=run('drive',sil,100,60,100,100); chk(abs(y).max()==0,'silence in, silence out')
y,e=run('drive',quiet,100,100,100,100); chk(rms(y)>rms(quiet)*3,f'a quiet note is lifted: rms {rms(quiet):.0f} -> {rms(y):.0f}')
a,_=run('drive',mid,60,0,100,100); b,_=run('drive',mid,60,100,100,100); chk(thd(a)<thd(b),f'tone 0 darker than 100 (harmonics {thd(a):.3f} vs {thd(b):.3f})')
print("CRUSH")
y,e=run('crush',mid,16,1,100); chk(np.array_equal(y,mid.astype(float)),'16 bit, rate 1 = the input, bit for bit')
for b in (2,4,8):
    y,e=run('crush',mid,b,1,100); lv=len(np.unique(y[:,0])); chk(lv<=2**b+1,f'{b} bits: {lv} levels (max {2**b+1}), peak {abs(y).max():.0f}')
y,e=run('crush',mid,16,4,100); r=y[:,0]; runs=np.diff(np.flatnonzero(np.diff(r)!=0)); chk(len(runs)>0 and np.median(runs)==4,f'rate 4: values held {int(np.median(runs))} frames')
y,e=run('crush',mid,3,5,0); chk(np.array_equal(y,mid.astype(float)),'mix 0 = the clean sound')
y,e=run('crush',mid,3,7,100); chk(abs(y[FS//2:].mean())<200,f'no DC offset ({y[FS//2:].mean():.1f})')
y,e=run('crush',loud,2,40,100); chk(y is not None and abs(y).max()<=32767,'loud chord, 2 bits, rate 40: in range')
y,e=run('crush',sil,2,40,100); chk(abs(y).max()==0,'silence in, silence out (zero is a step), also at 2 bits')
y,e=run('crush',st(sine(30,200)),8,1,100); chk(abs(y).max()==0,'a signal under half a step goes to zero')
print("PHASER")
y,e=run('phaser',mid,25,70,40,0); chk(np.array_equal(y,mid.astype(float)),'mix 0 = the clean sound, bit for bit')
for fbk in (0,40,100):
    y,e=run('phaser',st(rng.normal(0,8000,FS*10).clip(-32000,32000)),25,100,fbk,50); chk(y is not None and abs(y).max()<=32767 and rms(y)<20000,f'noise 10 s, feedback {fbk}: peak {abs(y).max():.0f} rms {rms(y):.0f}')
n10=st(rng.normal(0,6000,FS*8).clip(-32000,32000)); y,e=run('phaser',n10,50,100,60,50)
chk(0.4<rms(y)/rms(n10)<1.6,f'level kept: rms in {rms(n10):.0f} out {rms(y):.0f}')
# notches move: spectrum of 0.25 s windows over 4 s, std over time per bin
def spec(x,i): s=np.abs(np.fft.rfft(x[i:i+8192,0]*np.hanning(8192))); return 20*np.log10(s+1)
S=np.array([spec(y,i) for i in range(0,len(y)-8192,2048)]); fr=np.fft.rfftfreq(8192,1/FS)
band=(fr>300)&(fr<8000); v=S[:,band].std(axis=0); chk(v.mean()>2.5,f'the spectrum moves over time: mean spread {v.mean():.1f} dB (steady comb would be ~0)')
dl=y[:,0]-y[:,1]; chk(rms(dl)/rms(y)>0.05,f'left and right differ: |L-R| {rms(dl)/rms(y):.2f} of the level')
imp=np.zeros((FS*3,2)); imp[:4000]=rng.normal(0,8000,(4000,2)); y,e=run('phaser',imp.astype(np.int16),50,100,100,100); chk(abs(y[FS*2:]).max()<=4,f"settles to a +-1-3 LSB rounding wobble (-80 dBFS) at feedback 100: tail after 2 s {abs(y[FS*2:]).max():.0f}")
print("TREMOLO")
y,e=run('tremolo',mid,40,60); env=np.array([rms(y[i:i+441]) for i in range(0,len(y)-441,441)]); chk(env.max()/max(env.min(),1)>2,f'level swings {env.min():.0f}..{env.max():.0f}')
print('\nALL OK' if ok else '\nFAILURES'); sys.exit(0 if ok else 1)
