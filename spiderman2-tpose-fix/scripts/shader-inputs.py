#!/usr/bin/env python3
# Lists the input semantics of dumped vertex shaders (needs shader-features.py next to it, as dxbc.py).
# Usage: python3 scripts/shader-inputs.py path/to/sm2fix-dumps
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(__file__))
import importlib.util as _u
_spec = _u.spec_from_file_location("dxbc", os.path.join(os.path.dirname(os.path.abspath(__file__)), "shader-features.py"))
_m = _u.module_from_spec(_spec); _spec.loader.exec_module(_m); parts = _m.parts
def sig_names(p):
    # ISG1: count, offset(8), then entries of 8 dwords: stream, nameoff, index, sysval, comptype, reg, mask|rwmask, minprec
    if 'ISG1' in p:
        d=p['ISG1']; n,_=struct.unpack_from('<II',d,0); out=[]
        for i in range(n):
            e=struct.unpack_from('<8I',d,8+i*32); off=e[1]
            name=d[off:d.index(b'\0',off)].decode(); out.append(f"{name}{e[2]}")
        return out
    if 'ISGN' in p:
        d=p['ISGN']; n,_=struct.unpack_from('<II',d,0); out=[]
        for i in range(n):
            e=struct.unpack_from('<6I',d,8+i*24); off=e[0]
            name=d[off:d.index(b'\0',off)].decode(); out.append(f"{name}{e[1]}")
        return out
    return []
d=sys.argv[1]; c=collections.Counter(); per={}
for f in sorted(os.listdir(d)):
    names=sig_names(parts(open(os.path.join(d,f),'rb').read()))
    per[f]=names; c[" ".join(names)]+=1
for k,v in c.most_common(40): print(f"{v:4d}  {k}")
skin=[f for f,n in per.items() if any('BLEND' in x.upper() or 'BONE' in x.upper() or 'SKIN' in x.upper() or 'JOINT' in x.upper() for x in n)]
print("skinning-input shaders:", len(skin), skin[:10])
