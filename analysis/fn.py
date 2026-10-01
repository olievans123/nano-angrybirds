#!/usr/bin/env python3
"""fn.py <file.c> <addr-or-name-substring> ... : print the decompiled function(s) whose header matches."""
import sys,re
src=open(sys.argv[1]).read()
parts=re.split(r'(?m)^(?=// ===== )',src)
for key in sys.argv[2:]:
    for p in parts:
        h=p.split('\n',1)[0]
        if key in h:
            print(p)
