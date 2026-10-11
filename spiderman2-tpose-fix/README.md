# Spider-Man 2 T-pose fix for Apple Silicon (M1–M4)

## Quick start (no technical knowledge needed)

1. On this repo's GitHub page, switch to the branch `claude/elegant-dijkstra-9yhhvx`.
   Click the green **Code** button, then **Download ZIP**. Double-click the ZIP to unzip it.
2. Quit Spider-Man 2.
3. Open the `spiderman2-tpose-fix` folder. **Right-click** `Spider-Man 2 Fix.command`
   and choose **Open**, then **Open** again. You only need to right-click the first
   time, because macOS blocks downloaded scripts until you approve them.
4. A Terminal window opens and finds the game for you. Type **1** and press Return.
5. Play the game. Is the T-pose gone?
   - Either way, quit from the in-game menu, run the file again and pick **3**.
     That saves a log to your Desktop. Send it to me.

That's it. To undo everything, run the file and pick **4**.

## Results so far

| Date | Mac | Test | Result |
| --- | --- | --- | --- |
| 2026-10-10 | M3 Max, macOS 27.0.1, CrossOver, game on external drive | #1 barrier fix, `mode = all` (361,019 barriers injected) | **Still T-poses.** A missing barrier inside a command list is ruled out. |
| 2026-10-10 | same | #2 DirectStorage off | **Still T-poses.** DirectStorage GPU decompression is ruled out. |
| 2026-10-10 | same | #3 deep test (`mode = max`): GPU idle after each of 35,306 submissions, barriers after 227,086 dispatches + 30,465 `ExecuteIndirect` | **Still T-poses.** Rules out every GPU sync/race theory. **0 failed pipelines.** The game never submits to its compute queue. |
| 2026-10-10 | same | #4 `spoof = sm65,...` | Game refuses to start: *"Shader Model 6.6 support not detected … Current GPU: AMD Compatibility Mode"*. SM 6.6 is a hard requirement. |
| 2026-10-10 | same | #4b `spoof = no_int64,no_fp16`, all 525 compute shaders dumped | **Still T-poses.** Shader analysis: **no compute shader uses 64-bit ints or native fp16** (only TypedUAVLoadAdditionalFormats ×52, WaveOps ×8, descriptor-heap indexing ×4). The vkd3d target `324071d329f05ccc` is in the dump; it's a 3D-texture copy, unrelated to animation. |
| 2026-10-10 | same | #5 `minmax = linear` + vertex shader dump | **Still T-poses.** The game uses **no** MIN/MAX samplers (24 root signatures, 1,713 heap samplers, 0 min/max). Ruled out. |
| 2026-10-10 | same | #6 `profile = 1` (~3.5 min) | **Skinning shader `3d50acdb…` created but dispatched 0 times**, while 197 other compute shaders ran ~1M times. |
| 2026-10-10 | same | #6 again, ray tracing ON | Still T-poses and **still 0 skinning dispatches**, even though RT started (DispatchRays command signatures, 802 async-compute submits). One launch in this period came up with Spider-Man's current suit **fixed**. |
| 2026-10-10 | same | #7 `cpu_count = 8` | Still T-poses, skinning still 0. **Inconclusive:** 41 threads already existed when d3d12.dll loaded, so the game's job system was sized before the shim could act. |
| 2026-10-10 | same | #8 `pso_delay_ms = 10` (4,096+ pipelines delayed, ~40 s slower load) | **Still T-poses**, skinning still 0. Slow loading isn't the trigger. |
| 2026-10-10 | same | #9 Rosetta instruction check | **Rosetta is conformant.** All FMA/F16C/AVX/AVX2/SSE4/BMI tests match Intel bit-for-bit in all MXCSR modes. `rcpps`/`rsqrtps` match Intel's exact values (within spec). All atomics, split-line `lock xadd`, `cmpxchg16b` and TSO litmus tests pass. Only `dpps` differs, and only in *which* NaN encoding it returns when inputs are NaN (Intel's default-NaN vs propagated payload): harmless, and AMD differs from Intel there too. CPU translation is ruled out. |
| 2026-10-10 | same | #10 `trace_queries = 1` | **Found a broken GPU clock.** D3DMetal answers `GetTimestampFrequency` with **60 Hz**, and **every resolved timestamp is 0**, across ~15,000 timestamp queries in a few minutes (1M-entry timestamp and occlusion query heaps). Occlusion results look normal (small non-zero sample counts). Skinning still 0 dispatches. |
| 2026-10-10 | same | #11 `fake_timestamps = 1` | Emulation worked: 16,163 resolves answered with sane QPC times (1–80 µs deltas, 10 MHz). **Still T-poses**, skinning still 0. The broken clock is a real D3DMetal bug (see `APPLE-FEEDBACK.md`), but it isn't this one's cause. |
| 2026-10-11 | same | #12 `probe_skin = 1` | The CPU skin-job launcher never has requests (+0x30 = 0, flag 0), so that path is idle. But the skinning manager's **queued-item count (+0x50) is 1,100–5,600 every frame**: work is queued and never reaches the GPU (skinning shader still 0 dispatches). A screenshot showed an NPC with T-posed jacket sleeves but animated arms, and Spider-Man upside down in bind pose: only meshes skinned by `CS_ModelSkinning` are stuck. |
| 2026-10-11 | same | #13 `skin_budget = 65536` | **Budget is fine.** `ModelSkinGroupMax` was already 22,528 (all 28 `RenderBudgets` values look sane); raised to 65,536 anyway. The skin dispatch function ran ~2,150 times, ~820 with up to ~5,000 queued items, never over capacity, and the skinning shader still got **0 D3D12 dispatches**. The work is lost between the engine's own dispatch call (render context vtable +0x148) and D3D12. |
| 2026-10-11 | same | #14 `probe_skin = 1` (window trace) | **Correction: the skinning shader does run.** Inside the game's skin dispatch function the shim saw `SetPipelineState(CS_ModelSkinning)` + `Dispatch` 1,599 times (up to ~6,300 groups). The "0 skinning dispatches" in every earlier profile was a **bug in the shim's profiler** (it looked the skinning entry up while sorting, so it missed it whenever the sort moved it); fixed. So CS_ModelSkinning runs on M1–M4, and the meshes it skins still show the bind pose. Its decompiled code is ~9,300 lines with 144 dynamically indexed local arrays, a classic trigger for GPU-compiler bugs. Engine shader slot 224 is created (handle `0x7e7b2390`). |
| 2026-10-11 | same | #15 `skip_skinning = 1` | **The skinning shader's output is what's drawn.** With its dispatches dropped, the T-posed parts turned into shards and spikes (the woman's body vanished, Spider-Man's head floated; screenshots). So on M1–M4 the shader runs, its output is used, and that output is the bind pose. Root bindings: a 24-constant root parameter + 5 descriptor tables. |
| 2026-10-11 | same | #16 GPU check (`gpu-test/`) | **The skinning shader is translated correctly.** On the M3 Max through D3DMetal, the game's own `CS_ModelSkinning` gave 37,155 bit-exact vertices + 34 off by rounding, 0 wrong, at every influence count (1–16), deterministic across runs; the array-free rewrite (#17) the same. So the "local arrays" theory is out, and the game must be feeding the shader different inputs than it would on other GPUs. (An earlier attempt crashed in Wine's Mac display driver; the check now runs with `winemac.drv` disabled.) |
| 2026-10-11 | same | #16b GPU check, pooled layouts | **Also correct** in every layout (views at offsets, one shared upload buffer, one shared GPU-only buffer). |
| 2026-10-11 | same | #18 descriptor tracking | The skinning dispatch binds: `t0` bones = 16 MB **upload-heap** structured buffer (stride 64); `t1` = 2 GB R32_UINT typed buffer; `t2` = SkinningSB (stride 32, 22,528); `u0` = 335 MB R32G32B32A32_UINT buffer (custom heap). The bone buffer's first matrices change over time (identity during loading, real rotations later): the palette data is live. The CPU skin-job queue only takes mesh records flagged bit 0 (RVA `0x2b41300`), so its 0 requests may be normal. |
| 2026-10-11 | same | #18b palette check | **Found the bind pose in the data.** While loading, all queued palettes are identity. During play, ~85% of the 4,200–5,800 queued mesh subsets per frame have animated palettes, but **410–830 per frame use palettes that are pure identity** (first 4 bones), and they stay identity. A mesh skinned with identity bones renders in its bind pose: the T-pose. The shader and the bone buffer are fine; some palettes are simply never written. |
| 2026-10-11 | same | #18c flags + job queue | **CPU-job theory out.** No queued mesh record has bit 0 set, and the job queue's capacity is 22,528. Identity palettes don't line up with any record flag (some animated ones carry bits 1/2). Still 400–860 identity palettes per frame during play. |
| 2026-10-11 | same | #18d identity recheck | **Never written, not late.** Every identity palette stayed identity at the 1st, 4th and 16th queue submission after the dispatch was recorded and a frame later (up to ~180 ms). A user screenshot shows the pattern: an NPC on a bench with head, hands and trousers posed (sitting) but his **jacket** in the bind pose. |
| 2026-10-11 | same | #18e record dumps | The remap-table field doesn't separate them. Sampled identity meshes come from several NPC models with mesh flag word (+0x14) `3`/`1`/`0x11`; sampled animated ones (one big model, likely the player) have `7`. |
| 2026-10-11 | same | #18f mesh histograms | No mesh flag separates broken from working meshes, but mesh +0x1c (detail level / sub-part) does strongly: ~3% identity at 0, ~40–50% at 5–15. The queueing function (RVA `0x2b41300`) adds a copy job per instance (`instance+0x20` CPU matrices → palette slot `instance+0x1c`), so the identity palettes come from **CPU-side matrices that were never computed**: the game skipped evaluating those instances' animation. |
| 2026-10-11 | same | #19 `force_anim = 1` | **No change.** The scan found and patched 6–99 `AnimControllerComponentPrius` objects per pass (MaxUpdateDistance was 200, NeverCull 0); no `AnimUpdateLODPrius`/`SimpleAnimComponentPrius` objects turned up. Identity palettes stayed at 456–635 per frame and the T-pose stayed. Distance/visibility culling of animation isn't the cause. |
| 2026-10-11 | same | #20 readback monitor | **Readbacks are fine.** 4 GPU→CPU buffers (196–606 KB, from buffers and textures), all 12–91% non-zero and changing every snapshot. |
| 2026-10-11 | same | #21 `trap_writer = 1`: guard page on an animated character's CPU bone matrices, log the first writer | **Only memory clears caught** (the per-frame allocator reuses the pages), not the animation code. Headless menu screenshot: faces (333–335-bone rigs) and jackets keep identity bones. The game's Ziva RT face/cloth deformation (CS_Ziva* compute shaders) does run. Reports online say the T-pose happens only on pre-M5 Macs because of missing GPU features. |
| 2026-10-11 | same | #22 GPU data-format check (`gpu-test/format_test.exe`): 40 typed-buffer formats decoded on the Mac's GPU vs the exact D3D12 values | **All 40 correct** on the M3 Max. D3DMetal reads Ziva's quantized data fine. |
| 2026-10-11 | same | #23 `ziva_cpu = reference / sse41 / avx2`: Ziva RT has Reference, SSE4.1 and AVX2 CPU code, picked from CPUID (RVA 0x295e6a0) into the vtable of the global at RVA 0xbca9df0 | **No change with any of the three.** Rosetta on the M3 Max reports SSE4.2, AVX, AVX2, FMA, F16C, BMI1/2, LZCNT, MOVBE (XCR0=7, no AVX-512); the game picks AVX2. |
| 2026-10-11 | same | #24 `capture = 1`: saves one real dispatch of each Ziva RT GPU shader (TensorSkinning, KernelFunctions, ScatterAdd, RBFInterpolate, EigenSkinning, AnimVertZivaApply) to `sm2fix-capture\` — root signature, root arguments, every buffer view's data before, every UAV after. `gpu-test/replay.exe` re-runs it on vkd3d-proton/lavapipe and compares (self-test: `gpu-test/capture-selftest.sh`). First run: refused, the game's bindless root signature wasn't usable (parser limits); now each shader's own resource list (PSV0) is mapped through the root signature (1.0/1.1/1.2, up to 1024 ranges), failed captures are retried. Second run: no root signature recorded at all -- D3DMetal serializes root signatures into native macOS memory that Wine's VirtualQuery can't see, so the readable() check rejected every blob (also why the root-signature dump never showed in earlier probes); the 60 s delay was skipped by an unsigned-compare bug. Both fixed. Third run (5 captures): in every one the Mac's UAV after the dispatch is identical to before it, while the reference computes values ~0.1-1% away (like the next frame). Either the capture's CopyBufferRegion (ordered only by a UAV barrier) ran before the dispatch's writes landed, or the Mac didn't write. Captures now copy with a compute shader (`shim/capcopy.hlsl`, root SRV/UAV, ordered by UAV barriers like the game's own passes) and restore the game's root signature, pipeline and root arguments. Fourth run: **Ziva's GPU math is correct on the M3.** RBFInterpolate matches (max diff 5e-6), ScatterAdd bit-identical, KernelFunctions and TensorSkinning match except 1 and 3 words at the very end, which is a capture artifact (buffers whose size isn't a multiple of 4 lost their last element; fixed). AnimVertZivaApply (blends Ziva positions into the vertex pool) couldn't be compared (2 GB pool). The pose the CPU uploads to Ziva is animated (real quaternions). |
| next | | #25 `capture_all = 1`: capture one direct dispatch of every compute shader (8 MB per view, 16 MB per capture, 250 MB total) and replay all of them; `replay.exe` lists every shader whose Mac output differs, flags wave-op shaders (wave size differs between GPUs), and marks captures with missing textures/huge buffers or dynamic samplers as incomplete. Goal: find a GPU result (e.g. culling/visibility read back by the CPU) that is wrong on M1-M4 and makes the CPU skip animating faces/clothes. ExecuteIndirect work isn't covered yet. First run: 49 captures (266 MB, 31 MB zipped): 8 match, 14 use dynamic samplers (skipped), 11 have nothing comparable, 7 incomplete, 6 "differ" -- all six (and one replay crash) read a constant buffer the capture missed (CBVs living in buffers no SRV/UAV points at), so the replay saw zero counts and did nothing. CBVs are now captured by GPU address; the comparison no longer treats integer data as floats. | pending (2nd run) |

**Static analysis of the skinning scheduler.** Code cross-references from the
`ModelSkinJobFunc` and `Skinning Fence` strings lead to the per-frame skin-job
launcher at RVA `0x2b42560`. It's called once per frame from RVA `0x2c78460` with
the skinning manager (a global at RVA `0xbf60300`). It launches CPU skin jobs
(`ModelSkinJobFunc`) only when the job-system flag at RVA `0xc2ca194` (the struct at
`0xc2ca170` + `0x24`) is zero and the request count at manager+`0x30` is non-zero.
Otherwise it just clears bit `0x10` on every pending entry. Test #12 patches the
launcher's first 15 bytes (three register saves, verified byte-for-byte before
patching) with a jump to a logger, then runs the original code through a
trampoline. It logs the request count, the flag and the manager state at first
calls and every 3 s.

**Where queued skinning work goes (test #13).** `CS_ModelSkinning` is shader 224 in
the game's shader table; its index is used in exactly one function, at RVA `0x2b08f00`.
That function walks the queued items (count at manager+`0x50`, the value test #12 saw
at 1,100–5,600), writes one 32-byte entry per 1,280 vertices into the **`SkinningSB`**
structured buffer, but only while `entry < SkinningSB.capacity`, and then dispatches
`min(entries, capacity)` thread groups. The capacity is set once, by a stub at RVA
`0x2b087c0`, from **`RenderBudgets.ModelSkinGroupMax`** ("the maximum number of gpu
thread group elements ModelSkinningGroupCB in the skinning structured buffer").
`RenderBudgets` lives in zero-initialised data and is filled from the game's scene
settings at runtime. If that budget is 0 (or tiny) on this configuration, every
compute-skinned mesh is silently dropped and keeps its bind pose. That matches both
the 0 dispatches and the mixed-pose screenshot. Test #13 hooks that stub, logs all 28
`RenderBudgets` values, raises `ModelSkinGroupMax` to at least 65,536 (a 2 MB
buffer), then calls the game's own create function. It also hooks the dispatch
function and logs, every 3 s, the queued count against the buffer capacity. The shim
reads every address from the stub's own instruction displacements, and patches
nothing unless the opcodes match.

**Why the output is the bind pose (theory under test in #16).** For every 32-vertex
block the shader reads a header (influences per vertex, bone base). One-influence
vertices take their bone index from a plain byte read. Vertices with 2–16 influences
unpack delta-encoded bone indices and 8-bit weights through small local arrays that
are indexed at runtime (`a[0] = word; a[1..3] = 0; … a[i] >> shift`), which DXC emits
as `alloca`s. If those dynamic array reads come back as 0, every influence lands on the
block's base bone, usually the root. The mesh then follows the character rigidly in its
bind pose: a T-pose that stays attached and turns upside down with Spider-Man.
Vertex-shader-skinned parts use the same bone palette and animate correctly, so the
bone data is fine. `gpu-test/skin_gpu_test.exe` reports wrong vertices broken down by
influence count, which will show whether that's the trigger.

**The corrected shader (`shader-fix/`).** The game's DXIL has 144 `alloca` arrays, each
written at constant indexes and read once (or twice) at a runtime index in the same
block. `unalloca.py` rewrites every such read as a chain of `select`s over the stored
values and deletes the arrays; `name_values.py` renames values so DXC's assembler
accepts the module; `dxa` re-assembles and signs it and `dxv` validates it
(`make-fix.sh` does all of it, reproducibly). On the reference path the result is
bit-identical to the original on every test vertex. The shim only swaps it in for the
exact original shader (by hash) and falls back to the original if pipeline creation
fails.

**Why the clock matters.** Apple GPUs before M5 can't sample timestamps at arbitrary
points in a command stream, so D3DMetal appears to stub D3D12 timestamps out on
Apple7–9 (60 Hz, zeros) and implement them on Apple10. The engine issues thousands
of timestamp queries ("RenderFrameTime Timestamps", `gpuTimeBegin`/`gpuTimeEnd`).
If any of the timing it derives feeds animation (blend-in from the bind pose, the
`AnimUpdateLOD` decisions or `RenderBudgets`), a zero GPU time would freeze
characters at their starting (bind) pose, which is the T-pose.

**Current theory.** Since CPU translation is ruled out, what differs between
M1–M4 and M5 is the GPU, and how D3DMetal emulates D3D12 features on each family.
The engine has an `AnimUpdateLODSystem` (how often each character's animation is
updated) and `RenderBudgets`, which consume values read back from the GPU
(occlusion visibility, GPU timestamps). If those come back as zeros or garbage on
Apple7–9, animation updates could be throttled to nothing and skinning frozen
(`AutoFreezeSkinning`), giving bind pose while characters still render. Test #10
records exactly what the game reads back.

**Static analysis of `Spider-Man2.exe`** (the user's copy, not committed). It isn't
packed (normal section entropy) and it statically imports `amd_ags_x64.dll`.
D3DMetal presents the GPU as "AMD Compatibility Mode", so AMD-specific paths are
in play. Engine strings show:

* Skinning can run on the GPU (`ComputeShaders/CS_ModelSkinning`, the
  `3d50acdb…` shader) or on the CPU (`ForceCpuSkinning`: "If set, the model will
  always use CPU skinning", plus "cpu skinning jobs"). `ForceCpuSkinning` is a
  per-model asset property, not a runtime switch.
* `AutoFreezeSkinning`: "Automatically freeze skinning when no animations are
  active (does nothing if already in bind)". `BindAfterAnimEnd`: "Force back to bind
  pose after animation end". So the engine intentionally stops skinning and shows the
  **bind pose** when it believes no animation is active, which matches 0 skinning
  dispatches plus a T-pose.
* RDTSC appears in 8 functions: profiler timestamps, plus one TSC calibration that
  averages 100 RDTSC/QPC ratios. So Apple's 24 MHz counter under Rosetta is
  calibrated correctly. Not the cause.
* Instruction census (capstone over all 527,161 `.pdata` functions, 17.3 M
  instructions, 605 mnemonics): 1.47 M VEX instructions, mostly scalar AVX
  (`vmovss`, `vmulss`, `vaddss`). Also ~2,100 FMA (mostly `vfmadd231ps/ss`), 81
  `vcvtph2ps` + 49 `vcvtps2ph` (F16C), **1,536 legacy `rsqrtps` + 692 `rcpps`** + 168
  AVX estimate forms, 102 `popcnt`, 15 `tzcnt`, 13 `crc32`, and no gathers.

**Test #9, a Rosetta conformance test** (`rosetta-test/`). It runs 222 instruction
tests (all FMA/F16C/estimate forms the game uses, with exact encodings via inline
asm where the compiler would pick another form, plus AVX/AVX2/SSE4/BMI). Each runs
under 3 MXCSR environments (default, FTZ+DAZ, round-down) × 3 input kinds (normal,
special values, raw bits) × 4,000 inputs. It also checks estimate precision against
the x86 bound (≤ 1.5×2⁻¹²), atomicity (`lock xadd` aligned/misaligned/split across
a cache line, `lock cmpxchg16b`), and TSO message-passing litmus tests (plain stores
and `movnti`+`sfence`). The same `rosetta_test.exe` generated `golden.txt` on a real
Intel Xeon (Wine), self-checks with 0 differences, and a corrupted-golden negative
control is caught down to the exact case.

**The CPU side never schedules skinning.** The GPU never gets skinning work,
even with ray tracing on (which needs posed bodies), and the outcome changes from
launch to launch. Both point at a race in the game's own (Rosetta-translated)
character-setup code, not at D3DMetal. Running without Rosetta isn't possible yet:
CrossOver's ARM64/FEX preview has no D3D12 until CrossOver 27 (early 2027). Test #7
attacks the race from inside the process instead. The shim patches the game's
imports of `GetSystemInfo`, `GetNativeSystemInfo`, `GetActiveProcessorCount`,
`GetMaximumProcessorCount`, `GetProcessAffinityMask` and
`GetLogicalProcessorInformation(Ex)` (plus the process affinity mask) in
`DllMain`, before the game's startup code runs. The game then sizes its job system
for fewer cores, which changes how setup overlaps with streaming. The log records
how many threads already existed when the shim loaded.

**Who T-poses (user report):** Spider-Man himself and most other characters.
Switching suits *sometimes* fixes Spider-Man, but at random.

**Skinning found.** Of 330 vertex shaders, 301 are vertex-pulling
(`SV_VertexID` + `SV_InstanceID` only) and none have blend-weight inputs. The one
with the most bone-like math draws hair strands. Body skinning is the compute
shader **`3d50acdb571dce22`** (727 KB decompiled). Each workgroup reads an
8-dword job descriptor, applies up to 8 bone influences per vertex with 3×4
matrices, and writes posed vertices (int16 positions, 10:10:10:2 normals) to a
buffer that rendering reads.

**Current lead: an initialization race on the CPU side.** A shader miscompile
would fail the same way every time. The random suit-switch fix instead points to
character setup losing a race, for example the animation/skinning binding being
created before its data is ready, and staying broken until the character is
rebuilt. Rosetta-translated code (the game *requires* AVX2 and F16C, both
translated by Rosetta) running slower on M1–M4 would make the game lose that race
far more often. Test #6 checks whether the skinning shader runs at all: rarely or
never means the CPU side decided not to skin; constantly means the inputs (bone
matrices) are wrong.

**Shader analysis so far.** The 525 compute shaders were decompiled with
[dxil-spirv](https://github.com/HansKristian-Work/dxil-spirv) (`--glsl`). Every DXBC
checksum verifies against `shim/sm2fix_dxbc.c`. No compute shader looks like
main-body skinning. The closest, `968dc65df96b6aa1`, attaches points to skinned
triangles with barycentric weights (hair/fur roots). So skinning most likely
happens in vertex shaders, which test #5 dumps.

**Why min/max samplers?** M5 (Apple10) is the first Apple GPU with hardware
sampler min/max reduction. D3D12 exposes it as `D3D12_FILTER_MINIMUM_*` and
`D3D12_FILTER_MAXIMUM_*`, and D3DMetal advertises it on M1–M4 (tiled resources
tier 2). Engines use it to build Hi-Z depth pyramids for occlusion culling. If
D3DMetal emulates it wrongly on older GPUs, culling and visibility results that
feed animation updates could go wrong on M1–M4 only. Test #5 counts these
samplers (static ones in root signatures and descriptor-heap ones), rewrites
them, and dumps vertex shaders either way.

**What test #3's log tells us.** D3DMetal on the M3 Max reports Shader Model 6.6,
wave ops (32 lanes), `Int64ShaderOps = 1`, `Native16BitShaderOpsSupported = 1`,
raytracing tier 1.1, resource binding tier 3, and **UMA = 0** (it presents as a
discrete GPU). The game creates command signatures for draw, indexed draw and
dispatch, and uses `ExecuteIndirect` heavily. Since order and timing are ruled out,
the remaining explanation is a shader that compiles but **computes the wrong
answer on Apple7–9 GPUs**. Most likely D3DMetal mistranslates an operation that
Apple10 runs natively. Test #4 hides the SM 6.6 / 64-bit integer / 16-bit float
features, so the game may switch to fallback shaders. It also dumps every compute
shader for offline disassembly.

**Current thinking.** Rosetta translates CPU code the same way on every Apple chip,
M5 included, so a Rosetta bug would hit M5 too. What changes on M5 is the GPU: a new
Metal family (Apple10). So the remaining suspects are:

1. **A race between GPU queues, or between the CPU and the GPU.** Test #1 only
   covered races inside one command list. Test #3's `max` mode forces each
   submission to finish before the next one starts. If the T-pose goes away, it's
   a sync bug, and we can narrow it to the specific queue.
2. **A shader that D3DMetal can't build on Apple7–9 GPUs** (for example skinning
   that needs a feature only Apple10 has). The game might then skip skinning and
   draw the bind pose. Test #3 logs every failed pipeline and saves its shader
   bytecode to `sm2fix-dumps/` so it can be disassembled.
3. **A different code path chosen from feature queries.** The log records every
   `CheckFeatureSupport` answer. If M5 reports a feature that M1–M4 don't, we can
   try spoofing the answer.

---

Marvel's Spider-Man 2 (PC, Steam app 2651280) runs on Macs through CrossOver,
Whisky or Apple's Game Porting Toolkit (GPTK). All of these use Apple's
**D3DMetal** to turn DirectX 12 into Metal. On M1–M4 Macs, characters often
stand frozen in a **T-pose**. On M5 Macs they animate normally.

This folder contains what I found about the cause, and a small drop-in fix to
test it: `d3d12.dll`. Nothing in the game files is hex-edited.

> **Status:** the fix is built and passes its tests: the unit tests, and an
> end-to-end test under Wine with a stand-in D3D12 driver. It has **not** been
> run against the real game on a real Mac yet. That's the next step, and it's
> yours (see [Test plan](#test-plan)).

---

## What's going on (short version)

1. **The game has a known synchronization bug.** It forgets a GPU *barrier*
   after one of its compute shaders. A barrier is the instruction that means
   "wait until the previous GPU job has finished writing before the next one
   reads." The Linux D3D12 layer vkd3d-proton has worked around this since
   March 2026. Commit `cc9d6cc7`, *"vkd3d: Add missing barrier in Spider Man 2"*,
   forces a barrier after the compute shader with hash `0x324071d329f05ccc`.
   The shader hash was found by an Intel Mesa driver developer ("djdeath").
2. **Missing barriers cause races, and races only show on some GPUs.** D3D12
   lets back-to-back compute dispatches overlap unless the game puts a barrier
   between them. Whether the race actually corrupts data depends on how each GPU
   and driver schedules the work. That explains why the game looks fine on most
   Windows PCs, and why Windows players still report odd T-poses and
   "Intel GPU only" bugs.
3. **D3DMetal has no per-game workaround for this.** It's closed source, and
   unlike vkd3d-proton it has no shader quirk table you can see or edit. On
   M1–M4, Metal appears to run the two dispatches concurrently, so the later one
   reads data that isn't finished yet. My best guess is that this is skinning or
   animation-pose data, so the mesh gets the bind pose, which is a **T-pose**.
4. **Why the M5 works:** the M5 GPU is a new Metal family (Apple10, with a new
   scheduler and second-gen Dynamic Caching). D3DMetal may also choose different
   code paths for it. Either change alters the timing enough that the race no
   longer loses. The game is still wrong; it just stops being visible.
5. **Why "keep switching suits" sometimes helps:** a suit swap rebuilds the
   character's rig and skinning buffers. That re-runs the racy work, and
   sometimes it wins the race. This is the community workaround on CodeWeavers.

**The fix:** `sm2fix` is a small `d3d12.dll` that goes in the game folder. It
sits between the game and D3DMetal and hashes every compute shader the game
creates, using the same hash function as vkd3d-proton. After each `Dispatch()`
of the bad shader, it records a global UAV barrier
(`D3D12_RESOURCE_BARRIER_TYPE_UAV`, `pResource = NULL`). This is the same fix
vkd3d-proton applies, moved one layer up so it works with any translation layer.

### How confident am I?

| Claim | Confidence | Basis |
| --- | --- | --- |
| The game has a missing-barrier bug | **High** | It's in vkd3d-proton's source with a specific shader hash (`libs/vkd3d/device_workarounds.c`). |
| That bug causes the Mac T-pose | **Medium** | It fits every symptom: per-GPU, timing-dependent, fixed by re-initializing, appears on Windows too. But the vkd3d commit doesn't name the visual symptom. |
| Hash `324071d329f05ccc` still matches today's game build | **Medium** | A game patch that recompiles the shader changes its hash. `mode = all` covers that case. |
| The shim's mechanics work | **High** | Unit tests plus an end-to-end test under Wine (below). |

### Other causes I ruled down, but you can test cheaply

* **DirectStorage GPU decompression.** The game streams assets through
  DirectStorage with GDeflate decompression on the GPU. If animation data
  decompresses late or wrong, you'd also get T-poses. Removing `dstorage.dll` and
  `dstoragecore.dll` makes the game decompress on the CPU instead. This is a
  well-known PC tweak that is safe and reversible.
* **Rosetta 2 AVX translation.** Guides recommend "Advertise AVX" in CrossOver.
  If the CPU-side animation code took a buggy AVX path under Rosetta, turning
  this off would change the result. This is unlikely, since M5 Macs use the same
  Rosetta, but it costs nothing to check.

---

## Test plan

You need a Mac with the game installed in CrossOver, Whisky or GPTK, a save
where the T-pose shows up, and this repo.

### 1. Install the shim in diagnostic mode (`all`)

`all` mode adds a barrier after **every** compute dispatch. It's a bit slower,
but it doesn't depend on the shader hash still matching.

```bash
cd spiderman2-tpose-fix
./scripts/install-macos.sh --bottle "Steam" --mode all      # CrossOver bottle name
# or
./scripts/install-macos.sh --prefix "/path/to/whisky/bottle" --wine "/path/to/wine64" --mode all
```

The script finds `Spider-Man2.exe` in the bottle and copies `d3d12.dll` and
`sm2fix.ini` next to it. It also sets a Wine DLL override
(`d3d12 = native,builtin`) that applies to Spider-Man2.exe only. If it can't set
the override itself, it prints the three clicks to do it in Wine Configuration.

A prebuilt DLL is in `prebuilt/`. To build it yourself, run `./build.sh`. That
needs `brew install mingw-w64` or `brew install zig`.

### 2. Play and check the log

Launch the game and go somewhere the T-pose appears. Then open
`sm2fix.log` in the game folder. You should see:

```
sm2fix loaded: mode=all ...
loading real d3d12 from C:\windows\system32\d3d12.dll -> 0000...
D3D12CreateDevice ok -> ...
hooked Device::CreateComputePipelineState ...
hooked CommandList::Dispatch ...
forced barriers so far: 1, 2, 4, 8 ... (powers of two)
```

* **No log file at all:** the game didn't load the shim. Check the DLL override.
* **`ERROR: could not load the real d3d12.dll`:** put the real path in
  `real_d3d12 =` in `sm2fix.ini`.

### 3. Read the result

| Result in `all` mode | What it means | Next step |
| --- | --- | --- |
| **T-pose is gone** | Confirmed: a missing compute barrier causes it. | Switch to `mode = hash` in `sm2fix.ini` to win back performance. If the T-pose comes back in `hash` mode, a game patch changed the shader hash. Set `log_shaders = 1`, send me the log, and we'll narrow it down. |
| T-pose remains, log shows barriers | Not a compute→compute race, or the game uses `ExecuteIndirect` dispatches, which this version doesn't cover. | Try step 4. |
| T-pose remains, no log | The shim never loaded, so this test told us nothing. | Fix the DLL override, then retry. |

### 4. Fallback experiments (each one is reversible)

1. **DirectStorage off:** in the game folder, rename `dstorage.dll` and
   `dstoragecore.dll` (for example add `.off` to the end). Test. Rename them back
   afterwards.
2. **AVX:** in CrossOver, toggle the bottle's *Advertise AVX* option. Elsewhere,
   set `ROSETTA_ADVERTISE_AVX=1`, or remove it if it's already set. Test both ways.

Send back what you saw and the `sm2fix.log`, and we'll take the next step from
there.

### Uninstall

```bash
./scripts/install-macos.sh --bottle "Steam" --uninstall
```

---

## Configuration (`sm2fix.ini`)

| Key | Default | Meaning |
| --- | --- | --- |
| `mode` | `hash` | `hash`: barrier only after the listed shaders. `all`: barrier after every compute dispatch, including `ExecuteIndirect`. `max`: `all`, plus wait for the GPU after every queue submission (slow, diagnostic only). `off`: log only. |
| `hashes` | `324071d329f05ccc` | Comma-separated hex shader hashes. Uses the vkd3d-proton hash, so its quirk-table entries work as-is. |
| `log_shaders` | `0` | `1` writes every compute shader hash to `sm2fix.log`. |
| `spoof` | *(empty)* | Feature answers to hide from the game: `sm65` (report SM 6.5), `no_int64`, `no_fp16`, `uma` (claim unified memory), `no_rt`. |
| `dump_compute` | `0` | `1` saves every compute shader to `sm2fix-dumps\`. |
| `dump_graphics` | `0` | `1` saves every vertex/mesh shader to `sm2fix-dumps\`. |
| `profile` | `0` | `1` counts dispatches per compute shader (by hash) and logs the top ones every 20 s and at exit, always including skinning shader `3d50acdb571dce22`. |
| `force_anim` | `0` | `1` scans memory (from ~20 s after start, then every 10–30 s) for the game's animation config objects and turns off animation distance/visibility culling and auto-freezing. Reads/writes go through Read/WriteProcessMemory so freed memory can't crash the game. |
| `replace_skinning` | `0` | `1` swaps `CS_ModelSkinning` for `sm2fix-skinning.dxil` next to the shim (the installer copies it from `shader-fix/`). |
| `skip_skinning` | `0` | Diagnostic: `1` drops every `Dispatch` made while `CS_ModelSkinning` is bound. |
| `skin_budget` | `0` | Raise `RenderBudgets.ModelSkinGroupMax` (the `SkinningSB` capacity, which caps `CS_ModelSkinning` work per frame) to at least this many entries before the game creates the buffer. Also logs the budgets and skin-dispatch calls vs. capacity. `probe_sb_rva`/`probe_dispatch_rva` override the stub and dispatch-function RVAs. |
| `probe_skin` | `0` | `1` hooks the game's skin-job launcher (only if its bytes match the analysed build) and logs requests, the job-system gate flag and manager state. `probe_skin_rva`/`probe_jobsys_rva` override the addresses. |
| `fake_timestamps` | `0` | `1` answers timestamp queries in the shim: QPC frequency, QPC time recorded at each `EndQuery(TIMESTAMP)`, written into the game's mapped resolve buffer; also `GetClockCalibration`. |
| `trace_queries` | `0` | `1` logs query heaps, query counts, timestamp frequency and calibration, and periodically decodes the values the game reads back (timestamp deltas in µs, occlusion counts, pipeline statistics). |
| `pso_delay_ms` | `0` | Sleep this long in every pipeline creation, to imitate a cold shader cache (slow first boot). |
| `cpu_count` | `0` | Report at most this many processors to the game (import patching + affinity mask). `0` = unchanged. |
| `minmax` | `keep` | Samplers with MIN/MAX reduction: `keep` (count only), `linear` (drop the reduction), `point` (point-sample). Root-signature blobs are edited on a copy and re-checksummed. |
| `real_d3d12` | *(empty)* | Path to the real `d3d12.dll`. Empty means `system32\d3d12.dll`. |

Environment variables `SM2FIX_MODE`, `SM2FIX_HASHES`, `SM2FIX_LOG_SHADERS` and
`SM2FIX_REAL_D3D12` override the file.

## How the shim works

* It exports the eight public `d3d12.dll` functions. Each one forwards to the
  real `d3d12.dll`, which is D3DMetal under CrossOver/GPTK and vkd3d on Linux.
* On `D3D12CreateDevice`, it patches the device's vtable slots for
  `CreateComputePipelineState`, `CreatePipelineState` and `CreateCommandList(1)`.
  It patches every interface version the device hands out. It does the same for
  each command list's `SetPipelineState`, `Reset`, `ClearState`, `Dispatch` and
  `Release`.
* At pipeline creation, it hashes the compute shader bytecode with vkd3d's FNV
  hash. For `CreatePipelineState` streams, it parses the subobject stream to find
  the CS.
* It tracks which pipeline is bound on each command list. After a `Dispatch`
  with a target pipeline, it records the UAV barrier. It never records one in
  bundles, because D3D12 forbids barriers there.
* Diagnostics, always on: failed pipeline and root-signature creation (failing
  shaders are dumped to `sm2fix-dumps/`), `CheckFeatureSupport` answers, command
  signatures, queue types, and counters at exit.
* Known gap: devices created through `ID3D12DeviceFactory`. The log warns if the
  game uses the factory.

## Development

```bash
./tests/run.sh        # native unit tests (hash, pipeline-stream parser, config)
./tests/e2e/run.sh    # builds the DLL + a mock driver + a fake game, runs them under Wine
./build.sh            # build/d3d12.dll
```

The end-to-end test checks these cases:

* A dispatch of the target pipeline gets exactly one UAV barrier, whether the
  pipeline was bound with `SetPipelineState`, `Reset`, or created from a pipeline
  stream.
* Other pipelines, cleared state and bundles get no barrier.
* `all` mode adds a barrier after every non-bundle dispatch, and `off` adds none.
* Hooking works on read-only vtables.

## Sources

* vkd3d-proton, *"vkd3d: Add missing barrier in Spider Man 2"* (commit `cc9d6cc7`, 2026-03-26), and the quirk table in `libs/vkd3d/device_workarounds.c`: <https://github.com/HansKristian-Work/vkd3d-proton>
* CodeWeavers, Spider-Man 2 "T-pose Fix" tip (keep switching suits): <https://www.codeweavers.com/compatibility/crossover/tips/marvels-spider-man-2/t-pose-fix>
* CodeWeavers forum, "T-Pose in Spiderman 2" (M4 MacBook Pro, suit switching didn't help): <https://www.codeweavers.com/compatibility/crossover/forum/marvels-spider-man-2?msg=342422>
* "Marvel's Spider-Man 2, Mac M5 Pro & M4, CrossOver, GPTK 4: Performance Test" (T-pose still on pre-M5): <https://www.youtube.com/watch?v=HHJfw9Nes7o>
* Apple Developer Forums, "broken animations and memory leak issues" (Spider-Man 2 T-pose under GPTK): <https://developer.apple.com/forums/thread/812854>
* Apple, Metal Feature Set Tables (M5 = Apple10, M4 = Apple9): <https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf>
* Microsoft, UAV barriers and overlapping dispatches: <https://learn.microsoft.com/en-us/windows/ai/directml/dml-barriers>
* Steam, "Voices out of sync, T-pose cinematics" (Windows): <https://steamcommunity.com/app/2651280/discussions/0/599643530220039064/>
* Intel Community, "Intel GPU bugs only, Marvel Spider-Man 2 bug": <https://community.intel.com/t5/Intel-ARC-Graphics/Intel-GPU-bugs-only-Marvel-Spider-Man-2-bug/td-p/1664619>
* Spider-Man 2 DirectStorage GPU decompression (PC Gamer / Digital Trends): <https://www.pcgamer.com/hardware/graphics-cards/spider-man-2s-pc-launch-woes-are-woe-ier-than-you-think-as-it-looks-like-gpu-powered-directstorage-is-hobbling-performance/>
