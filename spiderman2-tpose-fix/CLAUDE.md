# Spider-Man 2 T-Pose Fix

Fixes characters stuck in a T-pose in Marvel's Spider-Man 2 on M1–M4 Macs (CrossOver /
D3DMetal). Root cause: D3DMetal reads typed buffer views only up to element 2^28; the game
reads its 2 GB mesh pool through typed R32 views of ~536M elements. See README.md for the
investigation and `vendor-report/` for the reports to Apple and CodeWeavers.

The owner doesn't write code: explain changes in plain language, and keep things simple.

## Layout

| Path | What |
| --- | --- |
| `app/` | The Mac app users run (SwiftPM). `SM2FixKit` = all logic + tests; `SM2Fix` = SwiftUI; `CShaderFix` = C shader rewrite |
| `shim/fix.c` | The fix's `d3d12.dll` (Windows x64, ~450 lines). Shipped prebuilt in `prebuilt/fix/` |
| `shim/sm2fix.c` etc. | The diagnostic shim from the investigation (troubleshooting menu only, never shipped by the app) |
| `shader-fix/` | Reference Python scripts for the shader rewrite (the app uses the C port) |
| `gpu-test/`, `rosetta-*` | Investigation tests (D3D12 / CPU checks) |
| `vendor-report/` | Reports for Apple and CodeWeavers, with a standalone repro |
| `Spider-Man 2 Fix.command`, `scripts/` | The old Terminal troubleshooting menu |

## Commands

- App, first time and after changes (Mac): `app/dev-setup.sh` (checks tools, builds DXC once, tests, builds, opens the app)
- App tests only: `cd app && SM2FIX_DXC="$PWD/.dxc/bin" swift test`
- Fix DLL: `./build.sh` (needs `brew install zig` or mingw-w64), then copy `build/fix/d3d12.dll` to
  `prebuilt/fix/` and update `prebuilt/fix/d3d12.dll.sha256` (`shasum -a 256 d3d12.dll`)
- Fix DLL test (Linux or a Mac with Wine): `tests/fix/run.sh`
- Diagnostic shim tests: `tests/run.sh`, `tests/e2e/run.sh`, `tests/menu/run.sh`
- CI (`.github/workflows/sm2fix-app.yml`) runs the app tests, builds the universal app, self-tests the
  zip and renders screenshots into `app/screenshots/` (committed back by CI: pull before pushing)

## Rules

- **No game files in the repo or the app.** The app builds the two shaders on the user's Mac from
  their own `Spider-Man2.exe`, and checks them against the verified hashes in `PoolShader.spiderMan2`.
- **The app never starts Wine** and never kills processes. It edits the bottle's `user.reg` only when
  no `wineserver` is running; otherwise it changes nothing and asks the user to quit Steam/CrossOver.
- **All or nothing:** check everything before changing anything in the game folder or bottle.
- **The fix DLL stays minimal:** three device hooks, no threads, no game memory. It only acts when
  `sm2fix.ini`'s `game_build` matches the running exe and all listed shaders are present.
- Don't change `app/build-dxc.sh` casually: its hash is the CI cache key, and the pinned DXC commit
  must keep producing byte-identical shaders (`testRewriteMatchesTheReferenceBuild`).
- Public domain (Unlicense). Commit messages explain why, not just what.
