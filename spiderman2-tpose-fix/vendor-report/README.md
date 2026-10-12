# Vendor reports: the M1–M4 T-pose bug

This folder holds everything Apple and CodeWeavers need to understand, reproduce and fix
the Marvel's Spider-Man 2 T-pose bug on M1–M4 Macs.

| File | Purpose |
| --- | --- |
| [`APPLE.md`](APPLE.md) | Report for Apple (Feedback Assistant). The root cause is in D3DMetal. |
| [`CODEWEAVERS.md`](CODEWEAVERS.md) | Report for CodeWeavers (CrossOver support ticket). Covers the cause, the repro and the workaround. |
| `repro/` | Attachments for both: a standalone D3D12 test with source, a native Metal check, and results and logs from an M3 Max |

## How to file

1. **Apple first.** Open Feedback Assistant and create a new macOS report. Pick the Game
   Porting Toolkit / D3DMetal area if it's offered. Paste the title and body from
   `APPLE.md`, then attach a zip of this folder. Note the FB number you get back.
2. **Then CodeWeavers.** Open a support ticket from your CodeWeavers account. Paste
   `CODEWEAVERS.md`, fill in the FB number and your CrossOver version (CrossOver › About
   CrossOver), and attach the same zip.

Before sending, fill in the two placeholders in square brackets: the CrossOver version in
both reports, and the FB number in the CodeWeavers report.
