#!/bin/sh
# Synthetic DEVICE_LOST at Present / frame submission; not a real driver crash.
# Run from repo root after building build/test_vk_recovery with Vulkan headers.
# Requires a Vulkan device, shader compiler, and validation layer; optionally set VK_ICD_FILENAMES.
set -eu
T=$(mktemp -d); ok=0
cleanup() { if [ "$ok" = 0 ]; then echo "FAILED; logs in $T" >&2; else rm -rf "$T"; fi; }
trap cleanup EXIT
export ME1_RENDERER=vulkan ME1_GL_FAKEWIN=64x64 ME1_GEARMASS=custom ME1_GM_WORKERS=4 ME1_GM_QUALITY=0 ME1_VK_VALIDATE=1 ME1_VK_LOSTRESET=1
unset ME1_VK_FAKELOST ME1_VK_FAKELOST_SUBMIT ME1_VK_SEC_TEST_FAIL ME1_VK_ASYNC_PIPE || :
run() {
 mode=$1; sec=$2; ahead=$3; inject=$4; path=$5
 unset ME1_VK_FAKELOST ME1_VK_FAKELOST_SUBMIT || :
 case $inject in present) export ME1_VK_FAKELOST=2;; submit) export ME1_VK_FAKELOST_SUBMIT=2;; esac
 log="$T/${mode}-${sec}-${ahead}-${inject}-${path}.log"
 ME1_D3D_CMD=$mode ME1_VK_SECONDARY=$sec ME1_GM_AHEAD=$ahead ./build/test_vk_recovery "$path" >"$log" 2>&1
 grep -q 'recovery complete' "$log"
 grep -q 'validation layer: 0 messages' "$log"
 grep -Eq 'test_vk_recovery .* 0 failed' "$log"
 if grep -Eq 'validation:|texmem accounting went negative|FAIL ' "$log"; then echo "Validation or resource accounting failure: $log" >&2; return 1; fi
 echo "$mode $sec $ahead $inject $path: $(grep 'test_vk_recovery (' "$log" | tail -1)"
}
# Reset on each command-stream and secondary path, including two-frame render-ahead.
for mode in 0 1 2; do for sec in 0 1; do for ahead in 0 2; do for injection in present submit; do
 run "$mode" "$sec" "$ahead" "$injection" reset
done; done; done; done
# Test both automatic timeout paths at maximum render-ahead (6 seconds each).
for injection in present submit; do run 2 1 2 "$injection" timeout; done
ok=1
echo 'Synthetic recovery tests OK (actual device loss remains untested).'
