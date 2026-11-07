#!/bin/sh
# Run from the repository root, after building build/test_gl and build/test_cmd.
# Requires a Vulkan device with VK_LAYER_KHRONOS_validation and libme1spv.so.
# Optionally set VK_ICD_FILENAMES to a software Vulkan ICD.
set -eu
T=$(mktemp -d); ok=0
cleanup() { if [ "$ok" = 0 ]; then echo "FAILED; logs in $T" >&2; else rm -rf "$T"; fi; }
trap cleanup EXIT
export ME1_RENDERER=vulkan ME1_VK_VALIDATE=1 ME1_GEARMASS=custom ME1_GM_WORKERS=4 ME1_GM_QUALITY=0 ME1_GL_FAKEWIN=128x128
unset ME1_VK_SEC_TEST_FAIL ME1_VK_SPLIT ME1_VK_ASYNC_PIPE ME1_FRAME_DUMP ME1_FRAME_DUMP_EVERY || :
check() { grep -q 'validation layer: 0 messages' "$1"; grep -Eq '0 failed$' "$1"; }
crc() { sed -n 's/.*final frame crc \([0-9a-f]*\),.*/\1/p' "$1" | tail -1; }
for kind in base sec fail split async; do
 case $kind in
 base) export ME1_VK_SECONDARY=0;;
 *) export ME1_VK_SECONDARY=1;; esac
 unset ME1_VK_SEC_TEST_FAIL ME1_VK_SPLIT ME1_VK_ASYNC_PIPE || :
 case $kind in fail) export ME1_VK_SEC_TEST_FAIL=5;; split) export ME1_VK_SPLIT=5;; async) export ME1_VK_ASYNC_PIPE=1;; esac
 ME1_D3D_CMD=2 ./build/test_cmd 40 >"$T/$kind.log" 2>&1
 check "$T/$kind.log"
 [ "$kind" != fail ] || grep -q 'reverted to inline draws' "$T/$kind.log"
 [ "$kind" = base ] || { [ -n "$(crc "$T/base.log")" ] && [ "$(crc "$T/base.log")" = "$(crc "$T/$kind.log")" ]; }
 echo "$kind: $(grep 'test_cmd:' "$T/$kind.log" | tail -1)"
done
unset ME1_VK_SEC_TEST_FAIL ME1_VK_SPLIT ME1_VK_ASYNC_PIPE || :
ME1_VK_SECONDARY=1 ./build/test_gl >"$T/gl.log" 2>&1; check "$T/gl.log"
grep -Eq 'test_gl: [0-9]+ ok, 0 failed' "$T/gl.log"
ME1_VK_SECONDARY=1 ME1_VK_SEC_TEST_FAIL=1 ./build/test_gl >"$T/gl-fail.log" 2>&1; check "$T/gl-fail.log"
grep -q 'reverted to inline draws' "$T/gl-fail.log"
for mode in 0 1 2; do for workers in 0 4; do
 ME1_VK_SECONDARY=1 ME1_D3D_CMD=$mode ME1_GM_WORKERS=$workers ./build/test_cmd 12 >"$T/m${mode}w${workers}.log" 2>&1
 check "$T/m${mode}w${workers}.log"
 [ -n "$(crc "$T/m0w0.log")" ] && [ "$(crc "$T/m0w0.log")" = "$(crc "$T/m${mode}w${workers}.log")" ]
done; done
for kind in base sec fail; do
 case $kind in base) ME1_VK_SECONDARY=0;; *) ME1_VK_SECONDARY=1;; esac
 export ME1_VK_SECONDARY
 unset ME1_VK_SEC_TEST_FAIL || :
 [ "$kind" != fail ] || export ME1_VK_SEC_TEST_FAIL=3
 ME1_D3D_CMD=2 ./build/test_cmd 3 perf >"$T/perf-$kind.log" 2>&1
 check "$T/perf-$kind.log"
 [ "$kind" != fail ] || grep -q 'reverted to inline draws' "$T/perf-$kind.log"
 [ "$kind" = base ] || { [ -n "$(crc "$T/perf-base.log")" ] && [ "$(crc "$T/perf-base.log")" = "$(crc "$T/perf-$kind.log")" ]; }
done
grep -Eq 'secondary: .* [1-9][0-9]* multi-lane batches' "$T/perf-sec.log"
grep -Eq '[1-9][0-9]* parallel:' "$T/perf-sec.log"
unset ME1_VK_SEC_TEST_FAIL || :
for kind in base sec; do
 mkdir "$T/$kind"
 case $kind in base) ME1_VK_SECONDARY=0;; sec) ME1_VK_SECONDARY=1;; esac
 export ME1_VK_SECONDARY ME1_FRAME_DUMP_EVERY=1
 ME1_FRAME_DUMP="$T/$kind" ME1_D3D_CMD=2 ./build/test_cmd 12 >"$T/dump-$kind.log" 2>&1
 check "$T/dump-$kind.log"
 [ "$(find "$T/$kind" -name 'frame_*.png' | wc -l)" -eq 12 ]
done
diff -qr "$T/base" "$T/sec" >/dev/null
ok=1
echo 'secondary: GL, parallel draw batches, injected fallbacks, async pipeline, split and 12 identical frames OK'
