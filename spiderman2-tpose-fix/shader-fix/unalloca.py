#!/usr/bin/env python3
"""Removes the small local arrays from the game's CS_ModelSkinning shader.

DXC compiled the shader's runtime-indexed vectors/arrays into `alloca`s. Each one is used
in a single basic block as: N stores at constant indexes, then one load at a runtime
index. This rewrites every such load into a chain of `select`s over the stored values
(index 0 is the fallback) and deletes the array, its GEPs and stores. Nothing else in the
module changes. Any alloca that doesn't follow the pattern exactly aborts the rewrite.

    unalloca.py in.ll out.ll        (in.ll from: dxc -dumpbin shader.dxil -Fc in.ll)
"""
import re, sys

src = open(sys.argv[1]).read().split('\n')
alloca_re = re.compile(r'^  (%\d+) = alloca \[(\d+) x i32\], align 4$')
gep_re = re.compile(r'^  (%\d+) = getelementptr (?:inbounds )?\[(\d+) x i32\], \[\d+ x i32\]\* (%\d+), i32 0, i32 (%?\w+)$')
store_re = re.compile(r'^  store i32 (%?[-\w]+), i32\* (%\d+), align 4(?:, !(?:tbaa|noalias|alias.scope) !\d+)*$')
load_re = re.compile(r'^  (%\d+) = load i32, i32\* (%\d+), align 4(?:, !(?:tbaa|noalias|alias.scope) !\d+)*$')
label_re = re.compile(r'^(\d+|[\w.]+):|^define |^}')

allocas = {}
for i, line in enumerate(src):
    m = alloca_re.match(line)
    if m:
        allocas[m.group(1)] = int(m.group(2))
if not allocas:
    sys.exit('no allocas found')

# Lifetime markers (bitcast to i8* + llvm.lifetime.start/end) are only hints: drop them.
bitcast_re = re.compile(r'^  (%\d+) = bitcast \[\d+ x i32\]\* (%\d+) to i8\*$')
lifetime_re = re.compile(r'^  call void @llvm\.lifetime\.(start|end)\(i64 \d+, i8\* (%\d+)\) #\d+(, !noalias !\d+)?$')
casts = {}
for i, line in enumerate(src):
    m = bitcast_re.match(line)
    if m and m.group(2) in allocas:
        casts[m.group(1)] = i
pre_drop = set(casts.values())
for i, line in enumerate(src):
    m = lifetime_re.match(line)
    if m and m.group(2) in casts:
        pre_drop.add(i)
for c in casts:
    for i, line in enumerate(src):
        if i not in pre_drop and re.search(re.escape(c) + r'(?!\d)', line):
            sys.exit('lifetime cast used elsewhere: ' + line)
src = ['' if i in pre_drop else line for i, line in enumerate(src)]

geps = {}       # gep name -> (alloca, index text, line)
uses = {a: [] for a in allocas}
for i, line in enumerate(src):
    m = gep_re.match(line)
    if m and m.group(3) in allocas:
        geps[m.group(1)] = (m.group(3), m.group(4), i)
        uses[m.group(3)].append(i)
    elif any(re.search(re.escape(a) + r'\b', line) for a in allocas if a in line) and not alloca_re.match(line):
        names = [a for a in allocas if re.search(re.escape(a) + r'(?!\d)', line)]
        if names and not gep_re.match(line):
            sys.exit('unexpected use of %s: %s' % (names, line))

# Each GEP must be used by exactly one store (constant index) or one load (dynamic index).
gep_user = {}
for i, line in enumerate(src):
    for rx, kind in ((store_re, 'store'), (load_re, 'load')):
        m = rx.match(line)
        if m:
            ptr = m.group(2)
            if ptr in geps:
                if ptr in gep_user:
                    sys.exit('GEP used twice: ' + ptr)
                gep_user[ptr] = (kind, i, m.group(1))
for g, (a, idx, li) in geps.items():
    if g not in gep_user:
        sys.exit('GEP without a store/load: ' + src[li])
    # any other mention of the GEP name (e.g. passed to a call) is unsupported
    for i, line in enumerate(src):
        if i not in (li, gep_user[g][1]) and re.search(re.escape(g) + r'(?!\d)', line):
            sys.exit('GEP used elsewhere: ' + line)

def block_of(i):
    while i > 0 and not label_re.match(src[i]):
        i -= 1
    return i

drop = set()
insert = {}     # line index -> replacement lines
counter = [0]

def fresh():
    counter[0] += 1
    return '%%sm2u%d' % counter[0]

rewritten = 0
for g, (a, idx, li) in sorted(geps.items(), key=lambda kv: kv[1][2]):
    kind, ui, val = gep_user[g]
    if kind != 'load':
        continue
    if not idx.startswith('%'):
        sys.exit('constant-index load not handled: ' + src[ui])
    n = allocas[a]
    # The values stored before this load, in the same block, for each constant index.
    current = {}
    blk = block_of(ui)
    for g2, (a2, idx2, li2) in geps.items():
        if a2 != a or gep_user[g2][0] != 'store':
            continue
        si = gep_user[g2][1]
        if si < ui and block_of(si) == blk and li2 < ui:
            if idx2.startswith('%'):
                sys.exit('dynamic-index store not handled: ' + src[si])
            k = int(idx2)
            # keep the latest store before the load
            if k not in current or current[k][0] < si:
                current[k] = (si, gep_user[g2][2])
    if sorted(current) != list(range(n)):
        sys.exit('load of %s without all %d elements stored first in its block (have %s)' % (a, n, sorted(current)))
    # select chain: start from element 0, then idx == k ? v_k : acc
    lines = []
    acc = current[0][1]
    for k in range(1, n):
        c, s = fresh(), fresh()
        lines.append('  %s = icmp eq i32 %s, %d' % (c, idx, k))
        lines.append('  %s = select i1 %s, i32 %s, i32 %s' % (s, c, current[k][1], acc))
        acc = s
    lines.append('  %s = or i32 %s, 0' % (val, acc))
    insert[ui] = lines
    drop.add(li)
    rewritten += 1

# Drop every store/GEP/alloca of the arrays.
for g, (a, idx, li) in geps.items():
    drop.add(li)
    drop.add(gep_user[g][1])
for i, line in enumerate(src):
    m = alloca_re.match(line)
    if m:
        drop.add(i)

out = []
for i, line in enumerate(src):
    if i in insert:
        out.extend(insert[i])
    elif i not in drop:
        out.append(line)
open(sys.argv[2], 'w').write('\n'.join(out))
print('removed %d allocas, rewrote %d dynamic loads' % (len(allocas), rewritten))
