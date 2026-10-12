"""Rewrites a DXIL shader (LLVM IR text from `dxc -dumpbin`) so that one typed
Buffer<uint> SRV is read as a raw ByteAddressBuffer instead.

Why: D3DMetal on M1-M4 can't read typed buffer views past 2^28 elements (Metal's texture
buffer limit) -- those reads return 0 -- while raw views work to the end. The game reads
its 2 GB vertex/morph pool through typed R32_UINT views of ~536M elements, so meshes stored
in the upper half of the pool lose their skinning/morph data. With the shader reading the
pool raw (and sm2fix creating that view as a raw view), every element is reachable.

  python3 typed2raw.py in.ll out.ll <space> <register>

Changes: the resource's metadata kind TypedBuffer (10) -> RawBuffer (11); its handles'
annotations {10, 261} -> {11, 0}; every bufferLoad(index) on them -> rawBufferLoad(index*4)."""
import re, sys

src, dst, space, reg = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
text = open(src).read()
lines = text.split('\n')

# 1. resource metadata: !N = !{i32 id, <type>* undef, !"", i32 space, i32 lb, i32 range, i32 10, i32 0, !M}
meta_re = re.compile(r'^(!\d+ = !\{i32 (\d+), (%[^*]+)\* undef, !"", i32 ' + str(space) + ', i32 ' + str(reg) + r', i32 1, )i32 10, i32 0, !\d+\}$')
res_id = None
for i, l in enumerate(lines):
    m = meta_re.match(l)
    if m:
        res_id = int(m.group(2))
        lines[i] = m.group(1) + 'i32 11, i32 0, null}'
        break
if res_id is None:
    sys.exit('typed SRV space %d register %d not found in the resource metadata' % (space, reg))

# 2. handles created for that binding (SRV class 0)
if reg == 0 and space == 0:
    bind_pat = r'%dx\.types\.ResBind (?:zeroinitializer|\{ i32 0, i32 0, i32 0, i8 0 \})'
else:
    bind_pat = r'%dx\.types\.ResBind \{ i32 ' + str(reg) + ', i32 ' + str(reg) + ', i32 ' + str(space) + r', i8 0 \}'
created = set()
for l in lines:
    m = re.match(r'\s*(%[\w.]+) = call %dx\.types\.Handle @dx\.op\.createHandleFromBinding\(i32 217, ' + bind_pat, l)
    if m:
        created.add(m.group(1))
if not created:
    sys.exit('no handle created for the binding')

annotated = set()
for i, l in enumerate(lines):
    m = re.match(r'(\s*(%[\w.]+) = call %dx\.types\.Handle @dx\.op\.annotateHandle\(i32 216, %dx\.types\.Handle (%[\w.]+), '
                 r'%dx\.types\.ResourceProperties )\{ i32 10, i32 261 \}(\).*)$', l)
    if m and m.group(3) in created:
        annotated.add(m.group(2))
        lines[i] = m.group(1) + '{ i32 11, i32 0 }' + m.group(4).replace('TypedBuffer<U32>', 'ByteAddressBuffer')

# 3. loads
out, n = [], 0
load_re = re.compile(r'^(\s*)(%[\w.]+) = call %dx\.types\.ResRet\.i32 @dx\.op\.bufferLoad\.i32\(i32 68, '
                     r'%dx\.types\.Handle (%[\w.]+), i32 ([^,]+), i32 undef\)(.*)$')
for l in lines:
    m = load_re.match(l)
    if m and m.group(3) in annotated:
        ind, val, h, idx, rest = m.groups()
        n += 1
        out.append('%s%%t2r%d = shl i32 %s, 2' % (ind, n, idx))
        out.append('%s%s = call %%dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32 139, %%dx.types.Handle %s, i32 %%t2r%d, '
                   'i32 undef, i8 1, i32 4)  ; RawBufferLoad(srv,index,elementOffset,mask,alignment)' % (ind, val, h, n))
    else:
        out.append(l)
if not n:
    sys.exit('no typed loads found on the handle')
text = '\n'.join(out)
if 'declare %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(' not in text:
    m = re.search(r'^declare %dx\.types\.ResRet\.i32 @dx\.op\.bufferLoad\.i32\(i32, %dx\.types\.Handle, i32, i32\) (#\d+)$', text, re.M)
    attr = m.group(1) if m else ''
    text = text.replace(m.group(0), m.group(0) + '\n\n; Function Attrs: nounwind readonly\n'
                        'declare %dx.types.ResRet.i32 @dx.op.rawBufferLoad.i32(i32, %dx.types.Handle, i32, i32, i8, i32) ' + attr)
# drop the typed-load declaration if nothing uses it any more (the validator rejects it)
if not re.search(r'call %dx\.types\.ResRet\.i32 @dx\.op\.bufferLoad\.i32\(', text):
    text = re.sub(r'\n(; Function Attrs:[^\n]*\n)?declare %dx\.types\.ResRet\.i32 @dx\.op\.bufferLoad\.i32\([^\n]*', '', text)
open(dst, 'w').write(text)
print('resource %d (space %d, t%d): %d handle(s), %d load(s) now raw' % (res_id, space, reg, len(annotated), n))
