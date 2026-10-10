# Unbound Fix

A small Mac app that fixes the startup black screen in **Need for Speed Unbound** (Steam build 16690907) when you play it in CrossOver with D3DMetal.

Drop the game folder into the window. On Steam it's named **Excalibur**, in `steamapps/common`. The app does the rest:

1. **Finds** `Engine.Render.Core2.PlatformPcDx12.dll`. You can drop the game folder, the DLL itself or your whole CrossOver bottle.
2. **Checks** the file's SHA-256 against the exact version the fix was made for. If it's any other version, it stops and changes nothing.
3. **Backs up** the original to `~/Library/Application Support/Unbound Fix/Backups` and checks the backup.
4. **Checks** that the 7 bytes at `0x1AB423` are `44 38 AB C0 01 00 00`, then replaces them with `80 A3 C0 01 00 00 00`.
5. **Checks** the result before saving, saves it safely (the file is never left half-written), then checks the saved file again. If anything doesn't match, it puts the original back.

The fix switches off an optional graphics cache involved in the startup crash.

After a fix, the window shows a checklist of exactly what was checked and changed. Errors are in plain English, with a **Copy Details** button for anything technical. **Check again** on the start screen re-checks your last game folder in one click, which is handy after a Steam update.

## Undo

Drop the game folder in again and click **Undo Fix**. It only restores the backup over a patched file, so it never overwrites a newer DLL from a game update. Steam's **Verify integrity of game files** also puts the original back.

A Steam update or file verification may remove the fix. Just run the app again. If the game updates the DLL, the app will say the version isn't supported and won't touch it.

## Install

Requires macOS 13 Ventura or later.

1. Open the latest run under [Actions → unbound-fix](../../actions/workflows/unbound-fix.yml) and download the **Unbound Fix** artifact.
2. Unzip it (you may need to unzip twice) and drag **Unbound Fix.app** into Applications.
3. The app isn't notarized, so macOS blocks it the first time. Open it once, click **Done**, then go to **System Settings → Privacy & Security**, scroll down and click **Open Anyway**.

Quit the game before using the app. Help is under the **?** button in the corner of the window.

## Build it yourself

Needs Xcode or the Command Line Tools.

```sh
cd UnboundFix
swift test                 # runs the safety tests
scripts/build-app.sh       # creates build/Unbound Fix.app
```

## Layout

| Path | What's there |
| --- | --- |
| `Sources/UnboundFixKit/Patcher.swift` | Finding, checking, backing up, patching and undoing |
| `Sources/UnboundFix` | The SwiftUI window |
| `Tests/UnboundFixKitTests` | Tests for every step on a stand-in file |
| `Tools/make_icon.py` | Draws the app icon into `Resources/AppIcon.iconset` |

CI also renders every screen in light and dark mode (the **Screenshots** artifact) so the UI can be reviewed without a Mac.
