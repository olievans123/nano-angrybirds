#!/usr/bin/env python3
"""dis.py <start_hex> <end_hex> : ARM (with VFP) disassembly of an address range of the AngryBirds 1.0
binary, annotating pc-relative literal loads with their value (float/double/string/symbol) and
bl targets with demangled names.  Uses arm-none-eabi-objdump on the raw __TEXT bytes (otool does not
decode ARMv6 VFP).  The full dump is cached in analysis/.dis2.txt."""
import sys,os,re,struct,subprocess
HERE=os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0,HERE)
from dat import word, cstr, SYMS, BIN
DIS=os.environ.get('DIS', os.path.join(HERE,'.dis2.txt'))
if not os.path.exists(DIS):
    tb=os.path.join(HERE,'.text.bin')
    open(tb,'wb').write(open(BIN,'rb').read()[:0x13e000])
    open(DIS,'w').write(subprocess.run(['arm-none-eabi-objdump','-D','-b','binary','-marm','--adjust-vma=0x1000',tb],capture_output=True,text=True).stdout)
a=int(sys.argv[1],16); b=int(sys.argv[2],16)
def lit(va, dbl=False):
    w=word(va)
    if w is None: return '?'
    if dbl:
        w2=word(va+4)
        return 'double %r'%struct.unpack('<d',struct.pack('<II',w,w2))[0]
    f=struct.unpack('<f',struct.pack('<I',w))[0]
    s='0x%x'%w
    if w in SYMS: s+=' &'+SYMS[w]
    c=cstr(w) if 0x130000<=w<0x13a1b8 else None
    if c is not None: s+=' "%s"'%c
    if 0x13f000<=w<0x153000:
        w2=word(w)
        if w2 in SYMS: s+=' ->&'+SYMS[w2]
    if 1e-6<abs(f)<1e9 or w==0x80000000: s+=' float %g'%f
    return s
for l in open(DIS):
    m=re.match(r'^\s*([0-9a-f]+):\t',l)
    if not m: continue
    va=int(m.group(1),16)
    if va<a: continue
    if va>=b: break
    l=l.rstrip('\n')
    m2=re.search(r'@ 0x([0-9a-f]+)\s*$',l)
    if 'pc' in l and m2 and ('ldr' in l or 'vldr' in l):
        t=int(m2.group(1),16)
        dbl=re.search(r'vldr\td\d+',l) is not None
        l+='\t; = %s'%lit(t,dbl)
    m3=re.search(r'\tbl?\w*\t0x([0-9a-f]+)',l)
    if m3:
        t=int(m3.group(1),16)
        if t in SYMS: l+='\t; '+SYMS[t]
    if va in SYMS: print('\n;;; '+SYMS[va])
    print(l)
