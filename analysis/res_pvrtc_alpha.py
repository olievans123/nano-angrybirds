#!/usr/bin/env python3
"""(resources-spec) Count PVRTC-4bpp blocks whose colour A/B words are flagged translucent (opaque bit clear)."""
import struct,glob,os
D='v1.0/Payload/AngryBirds.app/data/'
for f in sorted(glob.glob(D+'*.pvr')):
    b=open(f,'rb').read(); h=struct.unpack('<13I',b[:52])
    if h[4]&0xff!=0x19: continue
    w,hh=h[2],h[1]; n=(w//4)*(hh//4); ta=tb=0
    for i in range(n):
        mod,col=struct.unpack_from('<II',b,52+i*8)
        if not (col>>15)&1: ta+=1
        if not (col>>31)&1: tb+=1
    print('%-14s %dx%d level0 blocks=%d  colourA translucent=%d  colourB translucent=%d'%(os.path.basename(f),w,hh,n,ta,tb))
