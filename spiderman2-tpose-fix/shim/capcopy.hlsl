// sm2fix capture: copies `words` 32-bit words from a raw buffer (root SRV t0, or root UAV
// u1 when from_uav is set) into the shim's own buffer (root UAV u0) at byte dst_off.
// Run as a compute dispatch so it is ordered after the game's dispatch by a UAV barrier,
// exactly like the game's own compute work.
ByteAddressBuffer src : register(t0);
RWByteAddressBuffer dst : register(u0);
RWByteAddressBuffer src_uav : register(u1);
cbuffer args : register(b0) { uint words; uint dst_off; uint from_uav; uint stride; };
[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    for (uint i = id; i < words; i += stride)
        dst.Store(dst_off + i * 4, from_uav ? src_uav.Load(i * 4) : src.Load(i * 4));
}
