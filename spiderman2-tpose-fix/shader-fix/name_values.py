#!/usr/bin/env python3
"""Gives every unnamed local value and block in the (single) function a name, so the
LLVM 3.7 assembler in DXC accepts the module after instructions were removed.
%N -> %vN, '; <label>:N' -> 'vN:'. Globals (@), metadata (!) and attributes (#) untouched.

    name_values.py in.ll out.ll
"""
import re, sys

lines = open(sys.argv[1]).read().split('\n')
out, inside = [], False
local = re.compile(r'%(\d+)\b')
for line in lines:
    if line.startswith('define '):
        inside = True
        out.append(line)
        out.append('v0:')   # the entry block is implicitly %0
        continue
    if inside and line == '}':
        inside = False
    if inside:
        m = re.match(r'^; <label>:(\d+)(.*)$', line)
        if m:
            out.append('v%s:%s' % (m.group(1), re.sub(r'%(\d+)\b', r'%v\1', m.group(2))))
            continue
        out.append(local.sub(r'%v\1', line))
    else:
        out.append(line)
open(sys.argv[2], 'w').write('\n'.join(out))
