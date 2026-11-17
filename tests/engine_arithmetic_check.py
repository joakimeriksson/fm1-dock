#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Native ASan/UBSan regression for ARP/MiniPops fixed-point render arithmetic.

Requires a compiler with address/undefined sanitizers; failure is never suppressed.
No firmware build, package, USB or hardware command is run.
"""
import argparse
import os
from pathlib import Path
import shlex
import subprocess

PROGRAM = r'''
#include "arp_engine.h"
#include "arp_presets.h"
#include "mp_engine.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static int32_t out[512];
static int saw_audio;
static void check_audio(uint32_t n)
{
    for (uint32_t i=0;i<2u*n;i++) if (out[i]) saw_audio=1;
}
int main(void)
{
    const uint32_t notes[]={24,60,96};
    assert(ARP_SEQS[0].tempo>=40u);
    for (uint32_t p=0;p<ARP_N_PATCHES;p++) for (uint32_t k=0;k<3;k++) {
        arp_t a;arp_init(&a);arp_set_patch(&a,&ARP_PATCHES[p]);arp_note_on(&a,notes[k],100);
        for (uint32_t i=0,block=0;i<4096;block++) {
            uint32_t n=(block & 1u)?17u:64u;if (n>4096u-i)n=4096u-i;
            memset(out,0,sizeof out);arp_render(&a,out,n);check_audio(n);i+=n;
        }
        arp_note_off(&a,notes[k]);
        memset(out,0,sizeof out);arp_render(&a,out,256);
    }
    assert(saw_audio);puts("ARP: every factory patch at three pitches, short spans and release PASS");
    mp_t m;mp_init(&m);saw_audio=0;
    for (uint32_t v=0;v<MP_VOICES;v++)mp_trigger(&m,v,65536);
    assert(mp_take_hits(&m)==((1u<<MP_VOICES)-1u));
    for (uint32_t i=0,block=0;i<44100;block++) {
        uint32_t n=(block & 1u)?17u:256u;if(n>44100u-i)n=44100u-i;
        memset(out,0,sizeof out);mp_render(&m,out,n);check_audio(n);i+=n;
    }
    assert(saw_audio);
    mp_set_keys(&m,(1u<<MP_KEYS)-1u,1);mp_set_tempo(&m,240);mp_play(&m,1);
    for(uint32_t i=0;i<128;i++){memset(out,0,sizeof out);mp_render(&m,out,256);}
    mp_play(&m,0);memset(out,0,sizeof out);mp_render(&m,out,256);
    puts("MiniPops: all direct voices, combined rhythms, short spans and stop PASS");
    return 0;
}
'''

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root',type=Path,default=Path(__file__).resolve().parents[1])
    parser.add_argument('--out',type=Path)
    args=parser.parse_args()
    src=args.source_root.resolve();out=(args.out or src/'build/host/engine-arithmetic').resolve()
    out.mkdir(parents=True,exist_ok=True)
    source=out/'render_check.c';source.write_text(PROGRAM)
    binary=out/'render_check'
    command=shlex.split(os.environ.get('CC','cc'))+[
        '-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-pedantic',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer',
        '-I'+str(src/'modules/arp2600'),'-I'+str(src/'modules/minipops'),str(source),
        str(src/'modules/arp2600/arp_engine.c'),str(src/'modules/minipops/mp_engine.c'),'-o',str(binary)]
    print(shlex.join(command),flush=True)
    subprocess.run(command,check=True);subprocess.run([str(binary)],check=True)
    return 0

if __name__=='__main__':
    raise SystemExit(main())
