#!/usr/bin/env python3
"""(resources-spec) Check whether sheet images are stored premultiplied (max(r,g,b)<=a for every pixel)."""
import struct, zlib, glob, os, sys
D='v1.0/Payload/AngryBirds.app/data/'
def png_rgba(path):
    b=open(path,'rb').read(); p=8; idat=b''; 
    while p<len(b):
        n,t=struct.unpack('>I4s',b[p:p+8]); c=b[p+8:p+8+n]; p+=12+n
        if t==b'IHDR': w,h,bd,ct,_,_,il=struct.unpack('>IIBBBBB',c)
        elif t==b'IDAT': idat+=c
    raw=zlib.decompress(idat); bpp={6:4,2:3}[ct]; stride=w*bpp
    out=bytearray(); prev=bytearray(stride); q=0
    for y in range(h):
        f=raw[q]; line=bytearray(raw[q+1:q+1+stride]); q+=1+stride
        if f==1:
            for i in range(bpp,stride): line[i]=(line[i]+line[i-bpp])&255
        elif f==2:
            for i in range(stride): line[i]=(line[i]+prev[i])&255
        elif f==3:
            for i in range(stride):
                a=line[i-bpp] if i>=bpp else 0
                line[i]=(line[i]+((a+prev[i])>>1))&255
        elif f==4:
            for i in range(stride):
                a=line[i-bpp] if i>=bpp else 0; bb=prev[i]; c=prev[i-bpp] if i>=bpp else 0
                pa=abs(bb-c); pb=abs(a-c); pc=abs(a+bb-2*c)
                pr=a if (pa<=pb and pa<=pc) else (bb if pb<=pc else c)
                line[i]=(line[i]+pr)&255
        out+=line; prev=line
    return w,h,bpp,bytes(out),il
for f in sorted(glob.glob(D+'*.png')):
    w,h,bpp,px,il=png_rgba(f)
    if bpp==3: print('%-22s RGB (no alpha) interlace=%d'%(os.path.basename(f),il)); continue
    viol=0; semi=0; zero_a_nonzero_rgb=0; n=w*h
    for i in range(0,len(px),4):
        r,g,b,a=px[i:i+4]
        m=max(r,g,b)
        if m>a: viol+=1
        if 0<a<255: semi+=1
        if a==0 and m>0: zero_a_nonzero_rgb+=1
    print('%-22s %dx%d pixels=%d semi-transparent=%d  rgb>alpha violations=%d (%.2f%%)  a==0&&rgb!=0: %d interlace=%d'%(os.path.basename(f),w,h,n,semi,viol,100.0*viol/n,zero_a_nonzero_rgb,il))
for f in sorted(glob.glob(D+'*.pvr')):
    b=open(f,'rb').read(); hdr=struct.unpack('<13I',b[:52])
    if hdr[4]&0xff!=0x10: print('%-22s PVRTC (fmt 0x%x) skipped'%(os.path.basename(f),hdr[4]&0xff)); continue
    w,h=hdr[2],hdr[1]; viol=semi=0
    for (v,) in struct.iter_unpack('<H',b[52:52+w*h*2]):
        r=(v>>12)&15; g=(v>>8)&15; bl=(v>>4)&15; a=v&15
        if max(r,g,bl)>a: viol+=1
        if 0<a<15: semi+=1
    print('%-22s %dx%d RGBA4444 semi-transparent=%d rgb>alpha violations=%d (%.2f%%)'%(os.path.basename(f),w,h,semi,viol,100.0*viol/(w*h)))
