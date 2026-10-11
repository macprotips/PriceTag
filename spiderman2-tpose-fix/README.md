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
   - **Yes:** run the file again and pick **2** for better performance.
   - **No:** run the file again and pick **4**. That saves a log to your Desktop.
     Send it to me.

That's it. To undo everything, run the file and pick **5**.

## Results so far

| Date | Mac | Test | Result |
| --- | --- | --- | --- |
| 2026-10-10 | M3 Max, macOS 27.0.1, CrossOver, game on external drive | #1 barrier fix, `mode = all` (361,019 barriers injected) | **Still T-poses.** A missing barrier inside a command list is ruled out. |
| 2026-10-10 | same | #2 DirectStorage off | **Still T-poses.** DirectStorage GPU decompression is ruled out. |
| 2026-10-10 | same | #3 deep test (`mode = max`): GPU idle after each of 35,306 submissions, barriers after 227,086 dispatches + 30,465 `ExecuteIndirect` | **Still T-poses** (assumed; awaiting confirmation). Rules out every GPU sync/race theory. **0 failed pipelines.** The game never submits to its compute queue. |
| next | | #4 hide GPU features (`spoof = sm65,no_int64,no_fp16`), dump all compute shaders | pending |

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
