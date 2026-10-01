#!/usr/bin/env python3
"""vt.py <vtable_addr_hex> [nslots] : dump a vtable (slot offset is relative to the vptr = vtable+8)."""
import sys
sys.path.insert(0,'analysis')
from dat import word, SYMS
a=int(sys.argv[1],16); n=int(sys.argv[2]) if len(sys.argv)>2 else 60
for i in range(n):
    w=word(a+8+4*i)
    print('+0x%02x  %08x  %s'%(4*i,w,SYMS.get(w,'?')))
