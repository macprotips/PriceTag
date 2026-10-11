// Copies a typed buffer view element by element into a 4x32-bit output, so the
// GPU's decoding of the view's format can be compared with the exact expected values.
Buffer<float4> src : register(t0);
RWBuffer<float4> dst : register(u0);

[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    dst[id] = src[id];
}
