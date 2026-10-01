#!/usr/bin/env python3
"""Resolve Ghidra DAT_xxxxxxxx literal-pool words in the AngryBirds 1.0 ARMv6 binary.
usage: dat.py 0006a42c 0006a430 ...   (prints word, float interp, and string if it points to one)"""
import struct, sys
BIN='v1.0/Payload/AngryBirds.app/AngryBirds'
d=open(BIN,'rb').read()
TEXT=(0x1000,0x13e000,0); DATA=(0x13f000,0x14000,1302528, 28672)
def off(va):
    if TEXT[0]<=va<TEXT[0]+TEXT[1]: return va-0x1000
    if DATA[0]<=va<DATA[0]+DATA[3]: return va-DATA[0]+DATA[2]
    return None
def word(va):
    o=off(va)
    return None if o is None else struct.unpack_from('<I',d,o)[0]
def cstr(va):
    o=off(va)
    if o is None: return None
    e=d.find(b'\0',o)
    s=d[o:e]
    try:
        t=s.decode('utf-8')
    except: return None
    if all(31<ord(c)<127 or c in '\n\t\r' for c in t) and len(t)<200: return t
    return None
SYMS={}
def loadsyms():
    try:
        for l in open('decomp/INDEX.txt'):
            p=l.rstrip('\n').split('\t')
            if len(p)>=4: SYMS[int(p[0],16)]=p[3]
    except Exception as e: pass
loadsyms()
def describe(va):
    w=word(va)
    if w is None: return 'unmapped'
    out='%08x -> %08x'%(va,w)
    f=struct.unpack('<f',struct.pack('<I',w))[0]
    out+='  (float %g, int %d)'%(f, struct.unpack('<i',struct.pack('<I',w))[0])
    s=cstr(w) if 0x132ff0<=w<0x13a1b8 else None
    if s is not None: out+='  str=%r'%s
    if w in SYMS: out+='  sym=%s'%SYMS[w]
    if 0x13f000<=w<0x153000:
        w2=word(w)
        if w2 is not None:
            out+='  [data] *=%08x'%w2
            if w2 in SYMS: out+=' sym=%s'%SYMS[w2]
            s2=cstr(w2) if 0x132ff0<=w2<0x13a1b8 else None
            if s2 is not None: out+=' str=%r'%s2
    return out
if __name__=='__main__':
    for a in sys.argv[1:]:
        a=a.replace('DAT_','')
        print(describe(int(a,16)))
