#!/usr/bin/env python3
"""Print annotated ARM disassembly (VFP decoded) for an address range of AngryBirds 1.0.

usage: disfn.py START_HEX [END_HEX|+SIZE]     e.g. disfn.py 7eb0 +3948

Needs two cached disassemblies (created on first run in $TMPDIR/ab10dis):
  objdump -d --mattr=+vfp2 --no-show-raw-insn   (decodes VFP)
  otool -arch armv6 -tV                          (names import stubs)
Literal-pool loads (ldr/vldr [pc,#n]) are annotated with the word as hex/float/double/string/symbol,
bl targets with the demangled name from decomp/INDEX.txt or the import stub name.
"""
import os, re, struct, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, 'v1.0/Payload/AngryBirds.app/AngryBirds')
CACHE = os.path.join(tempfile.gettempdir(), 'ab10dis')
os.makedirs(CACHE, exist_ok=True)
D2 = os.path.join(CACHE, 'objdump.txt'); D1 = os.path.join(CACHE, 'otool.txt')
if not os.path.exists(D2):
    with open(D2, 'w') as f:
        subprocess.run(['objdump', '-d', '--mattr=+vfp2', '--no-show-raw-insn', BIN], stdout=f, stderr=subprocess.DEVNULL)
if not os.path.exists(D1):
    with open(D1, 'w') as f:
        subprocess.run(['otool', '-arch', 'armv6', '-tV', BIN], stdout=f, stderr=subprocess.DEVNULL)
data = open(BIN, 'rb').read()
TEXT = (0x1000, 0x13e000, 0); DATA = (0x13f000, 28672, 1302528)
def off(va):
    if TEXT[0] <= va < TEXT[0] + TEXT[1]: return va - 0x1000
    if DATA[0] <= va < DATA[0] + DATA[1]: return va - DATA[0] + DATA[2]
    return None
def word(va):
    o = off(va)
    return None if o is None else struct.unpack_from('<I', data, o)[0]
def cstr(va):
    o = off(va)
    if o is None: return None
    e = data.find(b'\0', o); s = data[o:e]
    if 0 < len(s) < 120 and all(32 <= c < 127 or c in (9, 10) for c in s): return s.decode()
    return None
syms = {}
for l in open(os.path.join(ROOT, 'decomp/INDEX.txt')):
    p = l.rstrip('\n').split('\t')
    if len(p) >= 4: syms[int(p[0], 16)] = p[3]
stubs = {}
for m in re.finditer(r'\b(?:bl|b|blx)\w*\s+0x([0-9a-f]+) @ symbol stub for: (\S+)', open(D1).read()):
    stubs[int(m.group(1), 16)] = m.group(2)
def desc(va, dbl=False):
    w = word(va)
    if w is None: return '?'
    if dbl:
        w2 = word(va + 4)
        return 'double %r' % struct.unpack('<d', struct.pack('<II', w, w2))[0]
    out = '0x%08x' % w
    f = struct.unpack('<f', struct.pack('<I', w))[0]
    if w == 0 or 1e-7 < abs(f) < 1e9: out += ' float %.9g' % f
    if w in syms: out += ' &' + syms[w]
    s = cstr(w) if w > 0x1000 else None
    if s is not None and w not in syms: out += ' "%s"' % s.replace('\n', '\\n')
    if 0x13f000 <= w < 0x146000:
        w2 = word(w)
        if w2 in syms: out += ' ->&' + syms[w2]
        elif w2 is not None: out += ' ->data 0x%x' % w2
    return out
a0 = int(sys.argv[1], 16)
a1 = a0 + int(sys.argv[2][1:], 0) if sys.argv[2].startswith('+') else int(sys.argv[2], 16)
for l in open(D2):
    m = re.match(r'\s+([0-9a-f]+):\s+(.*)', l)
    if not m: continue
    a = int(m.group(1), 16)
    if a < a0: continue
    if a >= a1: break
    ins = m.group(2).rstrip()
    ins = re.sub(r'\s+@ 0x[0-9a-f]+ <.*', '', ins)
    ins = re.sub(r' <[^>]*>', '', ins)
    ins = re.sub(r'\s+@ imm = .*', '', ins)
    note = ''
    pm = re.search(r'\[pc(?:, #(-?(?:0x)?[0-9a-f]+))?\]', ins)
    if pm:
        d = int(pm.group(1), 0) if pm.group(1) else 0
        t = a + 8 + d
        note = '   ; [%x] = %s' % (t, desc(t, dbl=bool(re.match(r'vldr\s+d', ins))))
    bm = re.match(r'(bl\w*|b\w*)\s+0x([0-9a-f]+)$', ins)
    if bm:
        t = int(bm.group(2), 16)
        if t in syms: note = '   ; ' + syms[t]
        elif t in stubs: note = '   ; stub ' + stubs[t]
    if ins.startswith('<unknown>'):
        w = word(a)
        CC = ['eq', 'ne', 'hs', 'lo', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', '', 'nv']
        if w is not None and (w & 0x0ffffff0) == 0x012fff10: ins = 'bx%s r%d' % (CC[w >> 28], w & 15)
        elif w is not None and (w & 0x0ffffff0) == 0x012fff30: ins = 'blx%s r%d' % (CC[w >> 28], w & 15)
        elif w == 0xe1a0e00f: ins = 'mov lr, pc'
        else: ins = '.word 0x%08x' % (w or 0)
    print('%8x: %s%s' % (a, ins.replace('\t', ' '), note))
