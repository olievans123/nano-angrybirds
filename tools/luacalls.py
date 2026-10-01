#!/usr/bin/env python3
"""For every call of a native (global or _G.x.y) in the game's bytecode: arg and result counts."""
import sys, glob, collections
src = open('tools/luaglobals.py').read().split("gets, sets")[0]
ns = {}; exec(src, ns)
calls = collections.defaultdict(collections.Counter)
for f in sys.argv[1:]:
    for p in ns['parse'](open(f, 'rb').read()):
        reg = {}
        for ins in p['code']:
            op, a, c, b = ins & 63, (ins >> 6) & 255, (ins >> 14) & 511, (ins >> 23) & 511
            bx = ins >> 14
            if op == 5: reg[a] = p['k'][bx]
            elif op == 6:
                base = reg.get(b); key = p['k'][c-256] if c >= 256 else None
                reg[a] = (base + '.' + key).replace('_G.', '') if base and isinstance(key, str) else None
            elif op == 0: reg[a] = reg.get(b)
            elif op in (28, 29):
                if reg.get(a): calls[reg[a]][(b - 1 if b else 'var', c - 1 if c else 'var')] += 1
                for r in list(reg):
                    if r >= a: reg.pop(r)
            else: reg[a] = None
want = sys.stdin.read().split()
for name in sorted(calls):
    if not want or name in want or name.split('.')[0] in want:
        print('%-28s %s' % (name, '  '.join('args=%s ret=%s x%d' % (k[0], k[1], v) for k, v in sorted(calls[name].items(), key=str))))
