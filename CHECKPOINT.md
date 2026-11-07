# Checkpoint: GEARMASS core count fix (Android)

## Issue
On Android, GEARMASS reports `2 cores (2 fast, 6 slow)` and `CPU X.X/2 cores` instead of 8 cores. `gm_init()` runs on an already-pinned thread whose affinity mask only covers fast cores (2), so `sched_getaffinity` returns 2 instead of the full online CPU count.

## Fix applied
- `runtime/hle_thread.c`: `cpu_avail_init()` now prefers `sysconf(_SC_NPROCESSORS_ONLN)` (system-wide online CPU count) instead of the (possibly restricted) thread affinity mask. Falls back to `sched_getaffinity` only if `sysconf` fails.

## TODO
- `runtime/gearmass.c`: `topology()` also calls `hle_cpu_avail()`; verify it now sees 8 cores. May also need to read `/sys/devices/system/cpu/online` directly as a cross-check.
- Build Linux (`build-linux.sh`) and run `test_gm` / `test_gm_thermal` / `test_gm_integration`.
- Build Android APK (`build-android.sh`) and verify on phone that log shows `8 cores` and `CPU X.X/8 cores`.

## Files modified
- `runtime/hle_thread.c` (cpu_avail_init)
- `CHECKPOINT.md` (this file)
