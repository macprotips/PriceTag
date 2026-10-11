#!/usr/bin/env python3
# Tallies the SFI0 feature flags of a folder of dumped DXBC shaders (sm2fix-dumps).
# Usage: python3 scripts/shader-features.py path/to/sm2fix-dumps
import struct, sys, os, collections
SFI0 = {0:"Doubles",1:"ComputeShadersPlusRawAndStructuredBuffers",2:"UAVsAtEveryStage",3:"64UAVs",4:"MinimumPrecision",
5:"11_1_DoubleExtensions",6:"11_1_ShaderExtensions",7:"Level9ComparisonFiltering",8:"TiledResources",9:"StencilRef",
10:"InnerCoverage",11:"TypedUAVLoadAdditionalFormats",12:"ROVs",13:"ViewportAndRTArrayIndex",14:"WaveOps",15:"Int64Ops",
16:"ViewID",17:"Barycentrics",18:"NativeLowPrecision",19:"ShadingRate",20:"Raytracing_Tier_1_1",21:"SamplerFeedback",
22:"AtomicInt64OnTypedResource",23:"AtomicInt64OnGroupShared",24:"DerivativesInMeshAndAmpShaders",
25:"ResourceDescriptorHeapIndexing",26:"SamplerDescriptorHeapIndexing",27:"Reserved",28:"AtomicInt64OnHeapResource",
29:"AdvancedTextureOps",30:"WriteableMSAATextures",31:"SampleCmpGradientOrBias",32:"ExtendedCommandInfo"}
def parts(data):
    assert data[:4]==b'DXBC', data[:4]
    n=struct.unpack_from('<I',data,28)[0]
    offs=struct.unpack_from('<%dI'%n,data,32)
    out={}
    for o in offs:
        fourcc=data[o:o+4].decode('latin1'); size=struct.unpack_from('<I',data,o+4)[0]
        out[fourcc]=data[o+8:o+8+size]
    return out
def fnv(data):
    h=0xcbf29ce484222325
    for b in data: h=((h*0x100000001b3)&0xffffffffffffffff)^b
    return h
if __name__=='__main__':
    d=sys.argv[1]
    cnt=collections.Counter(); partcnt=collections.Counter()
    for f in sorted(os.listdir(d)):
        data=open(os.path.join(d,f),'rb').read()
        p=parts(data)
        partcnt.update(p.keys())
        flags=struct.unpack('<Q',p['SFI0'][:8])[0] if 'SFI0' in p else 0
        for b,nm in SFI0.items():
            if flags>>b&1: cnt[nm]+=1
    print("parts:",dict(partcnt)); print("feature use across",len(os.listdir(d)),"compute shaders:")
    for k,v in cnt.most_common(): print(f"  {v:4d} {k}")
