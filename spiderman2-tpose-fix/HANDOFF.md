# Handoff: from the cloud session to local work (2026-10-12)

Read `CLAUDE.md` first (layout, commands, rules). This file is the current state.

## Where things stand

- **Root cause found and fixed.** D3DMetal on M1–M4 returns 0 for typed buffer reads past
  element 2^28; the game reads its 2 GB mesh pool through typed R32 views of 536,346,624
  elements. Verified on the owner's M3 Max (macOS 27.0.1, CrossOver, game on an external
  drive at `/Volumes/MPT/SteamLibrary`): T-pose gone.
- **That verification used the old diagnostic shim** (`shim/sm2fix.c` with `pool_fix = 1`).
  Since then the shipped fix was rebuilt:
  - `shim/fix.c`: a minimal `d3d12.dll` (3 hooks, no threads, acts only when `sm2fix.ini`'s
    `game_build` matches the running exe and all listed shaders are present).
  - The app builds the two shaders on the user's Mac from their own `Spider-Man2.exe`
    (bundled DXC + `app/Sources/CShaderFix`), checks them against verified hashes, never
    starts Wine, edits `user.reg` only when no `wineserver` runs, all-or-nothing.
  - All of this passes CI (33 app tests, self-test of the zipped app) and `tests/fix/run.sh`
    under Wine with a mock driver, but **has not yet been run with the real game.**
- The owner's Mac is set up: repo at `~/Developer/SpiderMan2Fix` (their separate
  `~/Developer/PriceTag` is a different checkout; leave it alone), `xcode-select` points at
  `Xcode-beta.app`, `app/.dxc` is built, and `app/dev-setup.sh` builds and opens the app.

## Next step: test the new version for real

1. Quit Spider-Man 2, Steam and CrossOver.
2. `cd app && ./dev-setup.sh` (builds and opens the app).
3. The app should show the game, the "Steam (CrossOver)" bottle and **Update available**
   (the old diagnostic DLL is still installed). Click **Update Fix**; it should take seconds.
4. Play near NPCs: no T-pose expected.
5. Check `sm2fix.log` in the game folder. Expected, roughly:
   ```
   Spider-Man 2 T-Pose Fix 1.1
   Game build xxxxxxxx-xxxxxxxx matches. Applying the fix (2 shaders).
   Replaced shader 14902794dd7134e4: ok
   Replaced shader 3d50acdb571dce22: ok
   Wide typed view (536346624 elements) created as a raw view
   Exit: 2 shaders replaced, 1 wide views made raw
   ```
6. After quitting CrossOver: `ps -axo pid,command | grep -i '[e]xplorer.exe'` prints nothing.

If the log says the game build doesn't match, or a shader file is missing, the fix stays
off by design; find out why before changing the rule.

## Open items (decisions are the owner's)

- **Vendor reports** (`vendor-report/`): ready to file. Fill in the CrossOver version (both
  reports) and the Apple FB number (CodeWeavers report). Confirm the "GPTK 4.0b2 per the
  Metal HUD" line in `APPLE.md`. File with Apple first. A rendered copy is at
  https://claude.ai/artifact/1do5cRK1tzkpcoSYNrrxuQ (private until shared).
- **Public download link**: CI uploads the app as a GitHub Actions artifact (needs a GitHub
  login). A public GitHub release was proposed and needs the owner's OK.
- **Git history**: the removed `shader-fix/*.dxil` (modified game shaders) are still in the
  branch history. Purging means rewriting history + force-push; owner hasn't decided.
- **`dev-setup.sh`**: could auto-detect an installed Xcode when `xcode-select` points at the
  Command Line Tools (XCTest only ships with Xcode). Offered, not done.
- **Distribution caveats**: the app is ad-hoc signed (not notarized), so first launch needs
  right-click › Open or System Settings › Open Anyway. The bundled DXC tools are arm64 only
  (fine: only Apple Silicon is affected).

## Working agreements

- The owner doesn't write code. Explain in plain language, keep it simple, do the work.
- Pull before starting; CI commits screenshots back to the branch after each push.
- Branch: `claude/elegant-dijkstra-9yhhvx`. Don't open a PR unless asked.
- Validate before pushing (app: `./dev-setup.sh`; fix DLL: `./build.sh` + `tests/fix/run.sh`
  where Wine is available; diagnostic shim: `tests/run.sh`).
