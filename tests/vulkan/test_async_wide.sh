#!/bin/sh
# Software/real Vulkan comparison of opt-in wide overlap; run after make test targets.
# Set VK_ICD_FILENAMES as needed. Validation and libme1spv.so are required.
set -eu
T=$(mktemp -d); ok=0
cleanup() { if [ "$ok" = 0 ]; then echo "FAILED; logs in $T" >&2; else rm -rf "$T"; fi; }
trap cleanup EXIT
export ME1_RENDERER=vulkan ME1_VK_VALIDATE=1 ME1_GEARMASS=custom ME1_GM_WORKERS=4 ME1_GM_QUALITY=0 ME1_GL_FAKEWIN=128x128
unset ME1_VK_SEC_TEST_FAIL ME1_VK_SPLIT ME1_VK_ASYNC_PIPE ME1_VK_PIPEIDLE ME1_FRAME_DUMP ME1_FRAME_DUMP_EVERY || :
check() { grep -q 'validation layer: 0 messages' "$1"; grep -Eq '0 failed$' "$1"; }
crc() { sed -n 's/.*final frame crc \([0-9a-f]*\),.*/\1/p' "$1" | tail -1; }
for kind in base async wide sec split fail idle; do
 unset ME1_VK_ASYNC_PIPE ME1_VK_PIPEIDLE ME1_VK_SPLIT ME1_VK_SEC_TEST_FAIL || :
 export ME1_VK_SECONDARY=0
 case $kind in async) export ME1_VK_ASYNC_PIPE=1;; wide) export ME1_VK_ASYNC_PIPE=2;; sec) export ME1_VK_ASYNC_PIPE=2 ME1_VK_SECONDARY=1;; split) export ME1_VK_ASYNC_PIPE=2 ME1_VK_SECONDARY=1 ME1_VK_SPLIT=5;; fail) export ME1_VK_ASYNC_PIPE=2 ME1_VK_SECONDARY=1 ME1_VK_SEC_TEST_FAIL=3;; idle) export ME1_VK_ASYNC_PIPE=2 ME1_VK_PIPEIDLE=1;; esac
 mkdir "$T/$kind"
 ME1_FRAME_DUMP_EVERY=1 ME1_FRAME_DUMP="$T/$kind" ME1_D3D_CMD=2 ./build/test_cmd 12 >"$T/$kind.log" 2>&1
 check "$T/$kind.log"
 [ "$(find "$T/$kind" -name 'frame_*.png' | wc -l)" -eq 12 ]
 [ "$kind" = base ] || diff -qr "$T/base" "$T/$kind" >/dev/null
 case $kind in wide|sec|split|fail) grep -Eq "async pipeline: [1-9][0-9]* jobs \([1-9][0-9]* wide\)" "$T/$kind.log"; grep -q 'async pipeline: wide draw preparation enabled' "$T/$kind.log";; idle) ! grep -q 'async pipeline: wide draw preparation enabled' "$T/$kind.log";; esac
 [ "$kind" != fail ] || grep -q "reverted to inline draws" "$T/$kind.log"
 echo "$kind: $(grep 'test_cmd:' "$T/$kind.log" | tail -1), CRC $(crc "$T/$kind.log")"
done
unset ME1_VK_PIPEIDLE ME1_VK_SPLIT ME1_VK_SEC_TEST_FAIL || :
for mode in 0 1 2; do
 ME1_D3D_CMD=$mode ME1_VK_SECONDARY=0 ME1_VK_ASYNC_PIPE=2 ./build/test_cmd 40 >"$T/cmd$mode.log" 2>&1
 check "$T/cmd$mode.log"; grep -q 'async pipeline: wide draw preparation enabled' "$T/cmd$mode.log"
 echo "cmd=$mode: $(grep 'test_cmd:' "$T/cmd$mode.log" | tail -1), CRC $(crc "$T/cmd$mode.log")"
done
ME1_VK_SECONDARY=0 ME1_VK_ASYNC_PIPE=2 ./build/test_gl >"$T/gl.log" 2>&1
check "$T/gl.log"; grep -q 'async pipeline: wide draw preparation enabled' "$T/gl.log"
echo "$(grep 'test_gl:' "$T/gl.log" | tail -1)"
for injection in present submit; do
 unset ME1_VK_FAKELOST ME1_VK_FAKELOST_SUBMIT || :
 case $injection in present) export ME1_VK_FAKELOST=2;; submit) export ME1_VK_FAKELOST_SUBMIT=2;; esac
 ME1_GL_FAKEWIN=64x64 ME1_VK_LOSTRESET=1 ME1_VK_SECONDARY=1 ME1_VK_ASYNC_PIPE=2 ME1_D3D_CMD=2 ./build/test_vk_recovery reset >"$T/recovery-$injection.log" 2>&1
 check "$T/recovery-$injection.log"; grep -q 'recovery complete' "$T/recovery-$injection.log"
 grep -q 'async pipeline: wide draw preparation enabled' "$T/recovery-$injection.log"
 echo "$injection: $(grep 'test_vk_recovery (' "$T/recovery-$injection.log" | tail -1)"
done
ok=1
echo 'wide async: frames, validation, draw modes and synthetic recovery OK; real device untested'
