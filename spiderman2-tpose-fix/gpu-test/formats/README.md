Tiny copy shaders for `format_test.exe` (`Buffer<T>[i]` -> `RWBuffer<T4>[i]`).
Rebuild with DXC: `dxc -T cs_6_0 -E main -Fo fmt_float.dxil fmt_float.hlsl` (same for int/uint).
