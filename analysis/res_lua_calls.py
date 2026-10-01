#!/usr/bin/env python3
"""(resources-spec) Show res.<fn> call sites in unluac output with N lines of following context.
usage: res_lua_calls.py <fn> [context=14] [max=6]"""
import sys,re
L=open('lua-src/gamelogic.lua').read().split('\n')
fn=sys.argv[1]; ctx=int(sys.argv[2]) if len(sys.argv)>2 else 14; mx=int(sys.argv[3]) if len(sys.argv)>3 else 6
n=0
for i,l in enumerate(L):
    if re.search(r'= L\d+_\d+\.res$',l) and i+1<len(L) and re.search(r'\.'+fn+r'$',L[i+1]):
        n+=1
        if n>mx: continue
        print('--- line %d'%(i+1))
        for k in range(max(0,i-1),min(len(L),i+ctx)): print('%6d %s'%(k+1,L[k]))
print('total',n)
