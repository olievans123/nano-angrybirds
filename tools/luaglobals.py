#!/usr/bin/env python3
"""Walk Lua 5.1 bytecode (float lua_Number, 32-bit) and report global reads/writes."""
import struct, sys, glob, collections
def parse(data):
    assert data[:5] == b'\x1bLuaQ'
    pos = [12]
    def u8(): v = data[pos[0]]; pos[0] += 1; return v
    def u32(): v = struct.unpack_from('<I', data, pos[0])[0]; pos[0] += 4; return v
    def s():
        n = u32()
        if n == 0: return None
        v = data[pos[0]:pos[0]+n-1]; pos[0] += n; return v.decode('latin1')
    protos = []
    def func():
        s(); u32(); u32(); u8(); u8(); u8(); u8()
        n = u32(); code = struct.unpack_from('<%dI' % n, data, pos[0]); pos[0] += 4*n
        k = []
        for _ in range(u32()):
            t = u8()
            if t == 0: k.append(None)
            elif t == 1: k.append(bool(u8()))
            elif t == 3: k.append(struct.unpack_from('<f', data, pos[0])[0]); pos[0] += 4
            elif t == 4: k.append(s())
            else: raise ValueError(t)
        p = {'code': code, 'k': k}
        protos.append(p)
        for _ in range(u32()): func()
        n = u32(); pos[0] += 4*n
        for _ in range(u32()): s(); u32(); u32()
        for _ in range(u32()): s()
    func()
    return protos
gets, sets, fields = collections.Counter(), collections.Counter(), collections.defaultdict(collections.Counter)
for f in sys.argv[1:]:
    for p in parse(open(f, 'rb').read()):
        reg = {}
        for ins in p['code']:
            op, a, c, b = ins & 63, (ins >> 6) & 255, (ins >> 14) & 511, (ins >> 23) & 511
            bx = ins >> 14
            if op == 5: gets[p['k'][bx]] += 1; reg[a] = p['k'][bx]
            elif op == 7: sets[p['k'][bx]] += 1
            elif op == 6:  # GETTABLE
                base = reg.get(b)
                key = p['k'][c-256] if c >= 256 else None
                if base and isinstance(key, str):
                    fields[base][key] += 1
                    reg[a] = base + '.' + key if base == '_G' else None
                else: reg[a] = None
            elif op == 11:  # SELF
                reg[a] = None
            elif op == 0: reg[a] = reg.get(b)
            else: reg[a] = None
native = sorted(g for g in gets if g not in sets)
print('GLOBALS READ BUT NEVER SET:', ' '.join('%s(%d)' % (g, gets[g]) for g in native))
for base in sorted(fields):
    if base in sets and not base.startswith('_G'): continue
    print('\n%s:' % base, ' '.join('%s(%d)' % kv for kv in sorted(fields[base].items())))
