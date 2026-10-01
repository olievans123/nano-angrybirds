#!/usr/bin/env python3
"""List float literal-pool constants loaded (vldr/ldr [pc]) by each Box2D function in the AngryBirds 1.0 binary,
to compare against b2Settings.h. usage: b2consts.py [name-substring ...]"""
import os, re, struct, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(tempfile.gettempdir(), 'ab10dis', 'objdump.txt')
if not os.path.exists(CACHE):
    subprocess.run([sys.executable, os.path.join(ROOT, 'analysis/disfn.py'), '2c78', '+4'], stdout=subprocess.DEVNULL)
data = open(os.path.join(ROOT, 'v1.0/Payload/AngryBirds.app/AngryBirds'), 'rb').read()
def word(va): return struct.unpack_from('<I', data, va - 0x1000)[0]
funcs = []
for l in open(os.path.join(ROOT, 'decomp/INDEX.txt')):
    p = l.rstrip('\n').split('\t')
    if len(p) >= 4 and re.match(r'(non-virtual thunk to )?b2', p[3]):
        funcs.append((int(p[0], 16), int(p[1]), p[3]))
pat = sys.argv[1:]
ins = {}
for l in open(CACHE):
    m = re.match(r'\s+([0-9a-f]+):\s+(.*)', l)
    if m: ins[int(m.group(1), 16)] = m.group(2)
for a, sz, name in funcs:
    if pat and not any(s in name for s in pat): continue
    vals = []
    for x in range(a, a + sz, 4):
        t = ins.get(x, '')
        m = re.match(r'(vldr\s+([sd])\d+|ldr\s+\w+), \[pc(?:, #(-?(?:0x)?[0-9a-f]+))?\]', t)
        if not m: continue
        tgt = x + 8 + (int(m.group(3), 0) if m.group(3) else 0)
        if m.group(2) == 'd':
            v = struct.unpack('<d', struct.pack('<II', word(tgt), word(tgt + 4)))[0]; vals.append('d:%r' % v)
        else:
            w = word(tgt); f = struct.unpack('<f', struct.pack('<I', w))[0]
            if w == 0 or 1e-9 < abs(f) < 1e12: vals.append('%.9g' % f)
    # immediates like mov rX,#0x3f800000 are not literal-pool; ignore
    seen = []
    for v in vals:
        if v not in seen: seen.append(v)
    print('%06x %-70s %s' % (a, name[:70], ' '.join(seen)))
