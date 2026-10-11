# Feedback Assistant report (ready to paste)

**Area:** Game Porting Toolkit / D3DMetal
**Title:** D3DMetal on M3 Max: GPU timestamp frequency 60 Hz and all-zero timestamps; Marvel's Spider-Man 2 characters stuck in bind pose (T-pose) on M1–M4, not on M5

**Environment**
- MacBook Pro, Apple M3 Max, macOS 27.0.1
- CrossOver (D3DMetal / Game Porting Toolkit 4.0b2 per the in-game HUD), Steam bottle
- Marvel's Spider-Man 2 (Steam 2651280), DirectX 12

**Problem 1: D3D12 timestamps are non-functional**
- `ID3D12CommandQueue::GetTimestampFrequency` returns **60**.
- Every `ResolveQueryData(D3D12_QUERY_TYPE_TIMESTAMP, …)` writes **0** to the
  destination buffer (thousands of queries per minute, measured with a D3D12
  interception layer that maps the resolve buffer).
- Occlusion queries resolve to plausible non-zero values in the same session.
- Expected: a real GPU tick rate and monotonically increasing timestamps (as on
  Windows drivers), or at least CPU-derived approximations.

**Problem 2: characters render in bind pose (T-pose) on M1–M4**
- Spider-Man and most NPCs render in their bind pose with distorted proportions.
  Switching suits occasionally fixes the player at random. Community reports say
  M5 Macs aren't affected.
- The game creates its GPU skinning compute pipeline (`CS_ModelSkinning`, DXIL
  hash FNV-1 `3d50acdb571dce22`) but **never dispatches it** (0 Dispatch/
  ExecuteIndirect over sessions with ~330k dispatches total). So the engine's CPU
  side never schedules skinning work.
- Ruled out by experiment (results in the attached README):
  - missing UAV barriers and cross-queue races (all submissions serialized, a
    barrier after every dispatch);
  - pipeline creation failures (none);
  - DirectStorage GPU decompression;
  - ray tracing on/off;
  - feature-report spoofing (Int64/FP16);
  - min/max samplers (the game uses none);
  - CPU core count;
  - shader-cache timing;
  - Rosetta 2 instruction translation (a 222-test AVX/AVX2/FMA/F16C/SSE4/BMI
    conformance test matches Intel bit-for-bit; atomics and TSO litmus tests
    pass);
  - faking correct timestamps.

**Steps to reproduce**
1. Install Marvel's Spider-Man 2 in a CrossOver bottle on an M1–M4 Mac.
2. Load any save. Spider-Man and most NPCs are in T-pose.
3. For problem 1, call `GetTimestampFrequency` / resolve timestamp queries from
   any D3D12 app.

**Attachments to include:** the `sm2fix-log-*.txt` files and this README.
