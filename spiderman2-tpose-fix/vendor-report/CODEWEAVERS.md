# Marvel's Spider-Man 2: characters stuck in T-pose on M1–M4

**For:** CodeWeavers, CrossOver support ticket or bug report

**Suggested title:** Marvel's Spider-Man 2 (Steam 2651280): characters in T-pose on M1–M4, caused by a D3DMetal typed buffer limit (repro and workaround attached)

---

## Summary

In Marvel's Spider-Man 2 on M1–M4 Macs, many characters render in their bind pose
(T-pose). We traced the cause to D3DMetal. Shader reads through a typed buffer view return
0 past element 268,435,456 (2^28), which is Metal's maximum texture buffer width. The
game reads its 2 GB character mesh pool through typed views of 536 million elements, so
every mesh stored above that point is never skinned.

This needs a D3DMetal fix from Apple, and we have filed the attached report with them.
Until then, a small workaround fixes the game in CrossOver today. We would like you to:

1. Pass the report to Apple through your D3DMetal contacts.
2. Consider offering the workaround to CrossOver users.

## Environment

| | |
| --- | --- |
| Mac | MacBook Pro, Apple M3 Max, macOS 27.0.1 |
| CrossOver | [version from CrossOver › About CrossOver] |
| Bottle | Steam bottle, D3DMetal enabled |
| Game | Marvel's Spider-Man 2, Steam app 2651280, Direct3D 12, installed in a Steam library on an external drive |
| Apple Feedback | FB[number] |

## Symptom

- Spider-Man and many NPCs appear in bind pose, often only partly. Upper garments stay
  in T-pose with a patch of skin at the neck while the head, hands and legs animate.
- Heads can appear detached or displaced, for example on the main menu.
- Reported on M1, M2, M3 and M4 Macs. M5 Macs are reportedly not affected.

## Cause, in short

The full analysis and data are in `APPLE.md`.

- The game creates `DXGI_FORMAT_R32_UINT` SRVs of 536,346,624 elements over a 2 GB buffer.
  Its skinning compute shader (`CS_ModelSkinning`) and a vertex morph shader read mesh
  data through them.
- D3DMetal creates these views without error, but reads at element 268,435,456 and above
  return 0. Raw and structured views of the same buffer read correctly.
- Metal's own validation rejects a texture buffer this wide: *"width (536854528) greater
  than the maximum allowed size of 268435456."*

## Reproduce without the game

The test needs only a bottle with D3DMetal and takes about a minute.

1. Unzip the attached `repro` folder anywhere on the Mac.
2. In Terminal:

   ```sh
   cd /path/to/repro
   "/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/bin/wine" \
       --bottle "<bottle name>" bigbuf_test.exe report.txt
   ```

   If the test exits before printing a table, add `WINEDLLOVERRIDES="winemac.drv=d"` in
   front of the command. On our machine, Wine's Mac display driver sometimes crashed this
   console-only test at startup.
3. Open `report.txt`. On M1–M4 the `R32_UINT` column shows `00000000*` from element
   268435456 on. The last line reads `RESULT: SOME VIEWS READ WRONG DATA AT LARGE INDICES`.
   Our output is in `repro/results/bigbuf-report-M3-Max.txt`.

You can also use **Run Command** in the CrossOver bottle menu: choose `bigbuf_test.exe`
and pass `report.txt` as the argument.

## Workaround (verified on M3 Max)

A `d3d12.dll` placed in the game folder sits between the game and D3DMetal. It passes
every call through unchanged, except:

1. **Large typed views.** A typed 32-bit buffer SRV with more than 2^28 elements is
   created as a raw view instead (`R32_TYPELESS` + `D3D12_BUFFER_SRV_FLAG_RAW`).
2. **Two shaders.** When the game creates the two compute shaders that read those views
   (DXIL hashes `3d50acdb571dce22` and `14902794dd7134e4`), the shim passes D3DMetal
   rewritten versions. In those versions the typed loads are replaced by equivalent raw
   loads at `index * 4`, and nothing else changes. They are validated and signed with the
   DXC validator. On vkd3d-proton, the rewritten skinning shader gives bit-identical
   results to the original.

**Result:** the T-pose is gone. The game log shows the conversion and replacement
working, with no pipeline failures in about 801,000 dispatches
(`repro/results/sm2fix-log-with-workaround-M3-Max.txt`).

**Installation.** The workaround needs four files in the game folder and one DLL
override in the bottle:

| What | Where |
| --- | --- |
| `d3d12.dll` (the shim) | game folder |
| `sm2fix-pool-3d50acdb571dce22.dxil` | game folder |
| `sm2fix-pool-14902794dd7134e4.dxil` | game folder |
| `sm2fix.ini` containing `mode = off` and `pool_fix = 1` | game folder |
| `d3d12` = `native,builtin` for `Spider-Man2.exe` only | `HKCU\Software\Wine\AppDefaults\Spider-Man2.exe\DllOverrides` |

The shim loads the real D3DMetal from `C:\windows\system32\d3d12.dll`. Users can install
it with a small Mac app that does the steps above and can remove them again. Source,
tests and the app are here:
<https://github.com/macprotips/PriceTag/tree/claude/elegant-dijkstra-9yhhvx/spiderman2-tpose-fix>

**Limits.** The fix applies only to this game: the shader replacements are matched by
hash, so a game update that changes those shaders turns the fix off silently. M5 Macs
don't need it, and on them it is harmless. The proper fix belongs in D3DMetal.

## What we're asking

1. **Forward to Apple.** Please pass `APPLE.md` and the `repro` folder to your D3DMetal
   contacts, or reference FB[number]. The repro is small and needs no game.
2. **CrossTie.** Consider a CrossTie, or a known-issue note in the compatibility database
   entry for Marvel's Spider-Man 2, that installs the workaround (the four files and the
   per-game DLL override above). Please contact us about redistribution terms first.
3. **Check other titles.** Other Direct3D 12 titles that read very large typed buffers
   will hit the same limit. The symptom is missing or zeroed data in the upper part of a
   large buffer, with no errors anywhere.

## Attachments

- `APPLE.md`: the full technical report as filed with Apple
- `repro/`: the D3D12 test with source, the native Metal check, and our results and logs
