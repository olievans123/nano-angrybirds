#!/usr/bin/env python3
"""Annotate Ghidra output: resolve DAT_xxxxxxxx literal-pool words to strings, symbols or floats."""
import re, struct, subprocess, sys, os, glob
BIN = 'v1.0/Payload/AngryBirds.app/AngryBirds'
data = open(BIN, 'rb').read()
segs = []   # (vmaddr, vmsize, fileoff, filesize)
out = subprocess.run(['otool', '-l', BIN], capture_output=True, text=True).stdout
cur = {}
for line in out.split('\n'):
    p = line.split()
    if len(p) == 2 and p[0] in ('vmaddr', 'vmsize', 'fileoff', 'filesize'):
        cur[p[0]] = int(p[1], 0)
        if p[0] == 'filesize':
            segs.append((cur['vmaddr'], cur['vmsize'], cur['fileoff'], cur['filesize']))
def off(addr):
    for va, vs, fo, fs in segs:
        if va <= addr < va + fs: return fo + addr - va
    return None
def word(addr):
    o = off(addr)
    return None if o is None else struct.unpack_from('<I', data, o)[0]
def cstr(addr):
    o = off(addr)
    if o is None: return None
    e = data.find(b'\0', o)
    s = data[o:e]
    if 0 < len(s) < 200 and all(32 <= c < 127 or c in (9, 10) for c in s): return s.decode()
    return None
syms = {}
nm = subprocess.run(['nm', BIN], capture_output=True, text=True).stdout
names = []
for line in nm.split('\n'):
    p = line.split(' ', 2)
    if len(p) == 3 and p[1] not in 'Uu':
        names.append((int(p[0], 16), p[2]))
dem = subprocess.run(['c++filt'], input='\n'.join(n for _, n in names), capture_output=True, text=True).stdout.split('\n')
for (a, _), d in zip(names, dem): syms.setdefault(a, d)
# non-lazy symbol pointers: indirect symbol table is awkward; resolve through the pointer's content instead
CSTR = (0x130978, 0x130978 + 0x71c6)
def describe(addr):
    v = word(addr)
    if v is None: return None
    s = cstr(v) if v > 0x1000 else None
    if s is not None and (v not in syms): return '"%s"' % s.replace('\n', '\\n').replace('*/', '* /')
    if v in syms: return '&' + syms[v]
    # pointer to a pointer (non-lazy pointer to global)
    if 0x13f000 <= v < 0x153000:
        v2 = word(v)
        if v2 in syms: return '&&' + syms[v2] + ' (ptr@%x)' % v
        return 'data@%x' % v
    f = struct.unpack('<f', struct.pack('<I', v))[0]
    if v == 0: return '0'
    if 1e-6 < abs(f) < 1e9: return '%.9gf' % f
    return '0x%x' % v
os.makedirs('decomp-annot', exist_ok=True)
for path in sys.argv[1:]:
    src = open(path).read()
    def sub(m):
        d = describe(int(m.group(1), 16))
        return m.group(0) if d is None else '%s/*%s*/' % (m.group(0), d)
    res = re.sub(r'\bDAT_([0-9a-f]{8})\b(?!/\*)', sub, src)
    open(os.path.join('decomp-annot', os.path.basename(path)), 'w').write(res)
