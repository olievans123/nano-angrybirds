#!/usr/bin/env python3
"""Build a Linux-syscall test wrapper around the nano library (no device access)."""
from pathlib import Path
import re
import subprocess
import sys
import json

sdk = Path(sys.argv[1]).resolve()
heap_mib = int(sys.argv[2]) if len(sys.argv) > 2 else 16
data_dir = sys.argv[3] if len(sys.argv) > 3 else 'build/data-small-pages'
event_file = sys.argv[4] if len(sys.argv) > 4 else 'tests/level1.txt'
frames = int(sys.argv[5]) if len(sys.argv) > 5 else 2300
jitter = len(sys.argv) > 6 and sys.argv[6] == 'jitter'
if not 4 <= heap_mib <= 32:
    raise SystemExit('test heap must be between 4 and 32 MiB')
port = Path(__file__).resolve().parents[2]
build = port / 'build/qemu'
build.mkdir(parents=True, exist_ok=True)
events = []
for line in (port / event_file).read_text().splitlines():
    fields = line.split()
    if fields[1] == 'shot':
        continue
    events.append('{%s,%d,%s,%s}' % (fields[0], fields[1] != 'up',
                                   fields[2] if len(fields) > 2 else 0,
                                   fields[3] if len(fields) > 3 else 0))
(build / 'events.h').write_text('static const struct Event events[] = {' + ','.join(events) + '};\n')
header = (sdk / 'sdk/gl.h').read_text()
header = re.sub(r'/\*.*?\*/', '', header, flags=re.S)
stubs = ['#include "gl.h"\n#include <stdlib.h>\n#include <stdio.h>\n',
         'static int draw_started;\n']
for ret, name, args in re.findall(r'\b(void|GLenum)\s+(gl\w+)\s*\(([^)]*)\)\s*;', header):
    body = 'return 0;' if ret == 'GLenum' else ''
    if name == 'glClear':
        body = 'draw_started = 0;'
    elif name == 'glDrawArrays':
        body = 'draw_started = 1;'
    elif name in ('glTexImage2D', 'glBufferData'):
        body = 'if(draw_started) { puts("GPU contract violation: upload after draw"); abort(); }'
        if name == 'glTexImage2D':
            body += 'if(width>256 || height>256 || width*height>32768 || width>4*height || height>4*width) { puts("GPU contract violation: oversized texture"); abort(); }'
    if name in ('glGenBuffers', 'glGenTextures'):
        pointer = args.split(',')[-1].strip().split()[-1].lstrip('*')
        body = 'static GLuint id; for(int i=0;i<n;i++) %s[i]=++id;' % pointer
    stubs.append('%s %s(%s) {%s}\n' % (ret, name, args, body))
(build / 'gl.c').write_text(''.join(stubs))
flags = ['-marm', '-mcpu=cortex-a5', '-mfloat-abi=softfp', '-mfpu=vfpv3-d16',
         '-DTEST_FRAMES=%d' % frames, '-DTEST_JITTER=%d' % jitter,
         '-DTEST_HEAP_BYTES=%d' % (heap_mib * 1024 * 1024),
         '-DTEST_DATA_DIR=' + json.dumps(data_dir),
         '-mword-relocations', '-fno-builtin', '-ffunction-sections', '-fdata-sections',
         '-O2', '-nostdlib', '-static', '-Wl,--gc-sections', '-Wl,-e,_start',
         '-I'+str(sdk/'sdk'), '-I'+str(port/'src'), '-I'+str(build)]
cc = 'arm-none-eabi-gcc'
libc = subprocess.check_output([cc, '-marm', '-march=armv5te+fp', '-mfloat-abi=softfp',
                                '-print-file-name=libc.a'], text=True).strip()
libgcc = subprocess.check_output([cc, *flags[:4], '-print-libgcc-file-name'], text=True).strip()
subprocess.run([cc, *flags, str(port/'tests/qemu/main.c'), str(build/'gl.c'),
                str(port/'build/nano/libab_nano.a'), libc, libgcc, '-o', str(build/'abtest')], check=True)
print(build/'abtest')
