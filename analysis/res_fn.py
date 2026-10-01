#!/usr/bin/env python3
"""(resources-spec helper) Print decompiled function(s) from decomp-annot/<file.c> with declaration noise filtered.
usage: res_fn.py <file.c> <addr-or-substring> [more keys...] [--raw]"""
import sys,re
root='decomp-annot/'
args=[a for a in sys.argv[1:] if a!='--raw']; raw='--raw' in sys.argv
f=args[0]
lines=open(root+f).read().split('\n')
starts=[i for i,l in enumerate(lines) if l.startswith('// =====')]
decl=re.compile(r'^(undefined\d?|int|uint|float|char|short|ushort|byte|bool|code|double|longlong|ulonglong|void)\s*\**\s*\w+(\s*\[\d+\])*;$')
for key in args[1:]:
    for n,i in enumerate(starts):
        if key in lines[i]:
            end=starts[n+1] if n+1<len(starts) else len(lines)
            for l in lines[i:end]:
                s=l.strip()
                if not raw and (s=='' or decl.match(s) or 'StringD1Ev' in s or 'Unwind_SjLj' in s): continue
                print(l)
            break
