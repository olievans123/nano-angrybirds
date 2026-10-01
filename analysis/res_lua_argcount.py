#!/usr/bin/env python3
"""(resources-spec) For each res.<fn> call site in gamelogic.lua, report the argument count and literal string args."""
import re,sys,collections
L=open('lua-src/gamelogic.lua').read().split('\n')
stats=collections.defaultdict(collections.Counter); lits=collections.defaultdict(lambda: collections.defaultdict(collections.Counter)); rets=collections.defaultdict(collections.Counter)
for i,l in enumerate(L):
    m=re.search(r'(L\d+_\d+) = L\d+_\d+\.res$',l)
    if not m or i+1>=len(L): continue
    m2=re.search(r'(L\d+_\d+) = L\d+_\d+\.(\w+)$',L[i+1])
    if not m2: continue
    reg,fn=m2.group(1),m2.group(2)
    # find call line
    assigned={}
    for k in range(i+2,min(len(L),i+80)):
        s=L[k].strip()
        ma=re.match(r'(L\d+_\d+) = (".*")$',s)
        if ma: assigned[ma.group(1)]=ma.group(2)
        mc=re.search(r'(?:^|= |return )'+re.escape(reg)+r'\((.*)\)$',s)
        if mc:
            args=[a.strip() for a in mc.group(1).split(',')] if mc.group(1).strip() else []
            stats[fn][len(args)]+=1
            for n,a in enumerate(args):
                if a in assigned: lits[fn][n+1][assigned[a]]+=1
            lhs=s.split('=')[0] if '=' in s.split(reg+'(')[0] else ''
            rets[fn][len([x for x in lhs.split(',') if x.strip()])]+=1
            break
for fn in sorted(stats):
    print(fn,'argcounts',dict(stats[fn]),'retcounts',dict(rets[fn]))
    if '-l' in sys.argv:
        for n in sorted(lits[fn]):
            items=lits[fn][n].most_common(12)
            print('   arg%d literals: %s%s'%(n,', '.join('%s x%d'%(k,v) for k,v in items),' ...' if len(lits[fn][n])>12 else ''))
