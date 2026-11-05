#!/bin/sh
# Builds libme1spv.so (GLSL -> SPIR-V for the Vulkan renderer) against the system glslang.
#   sh tools/build_spv_host.sh <out.so>      (called by make; CC, GLSLANG_CFLAGS, GLSLANG_LIBS, GLSLANG_LIBDIR may be set)
# Distros ship glslang differently: Debian/Ubuntu - only static .a in /usr/lib/<triplet>, Fedora/Arch/openSUSE - shared .so,
# self-built - /usr/local/lib*. Every variant is tried; if none links, the Vulkan renderer is skipped (OpenGL ES still works)
# and the build continues.
OUT=${1:?out.so}
SRC=android/jni/me1spv.c
CC=${CC:-cc}
LOG=${OUT%.so}.log
mkdir -p "$(dirname "$OUT")"; : > "$LOG"
try() {
    echo "== $*" >> "$LOG"
    $CC -O2 -shared -fPIC -fvisibility=hidden $GLSLANG_CFLAGS -o "$OUT.tmp" "$SRC" "$@" -lstdc++ -lm -lpthread -Wl,--no-undefined >> "$LOG" 2>&1 \
        && mv "$OUT.tmp" "$OUT" && { echo "libme1spv.so собрана (Vulkan-рендерер включён)"; exit 0; }
    rm -f "$OUT.tmp"
}
# 1. explicit
[ -n "$GLSLANG_LIBS" ] && try $GLSLANG_LIBS
# 2. static libraries (Debian/Ubuntu glslang-dev + spirv-tools, self-built)
TRIP=$($CC -print-multiarch 2>/dev/null); [ -n "$TRIP" ] || TRIP=$($CC -dumpmachine 2>/dev/null)
for d in $GLSLANG_LIBDIR /usr/lib/$TRIP /usr/lib64 /usr/lib /usr/local/lib /usr/local/lib64 /usr/local/lib/$TRIP; do
    [ -f "$d/libglslang.a" ] || continue
    A=""
    for l in glslang SPIRV MachineIndependent OSDependent GenericCodeGen OGLCompiler HLSL; do [ -f "$d/lib$l.a" ] && A="$A $d/lib$l.a"; done
    R=""; [ -f "$d/libglslang-default-resource-limits.a" ] && R="-Wl,--whole-archive $d/libglslang-default-resource-limits.a -Wl,--no-whole-archive"
    T=""; for l in SPIRV-Tools-opt SPIRV-Tools; do
        for t in "$d" /usr/lib/$TRIP /usr/lib64 /usr/lib /usr/local/lib /usr/local/lib64; do [ -f "$t/lib$l.a" ] && { T="$T $t/lib$l.a"; break; }; done
    done
    try $R -Wl,--start-group $A $T -Wl,--end-group
    try $R -Wl,--start-group $A -Wl,--end-group -lSPIRV-Tools-opt -lSPIRV-Tools
    try $R -Wl,--start-group $A -Wl,--end-group -lSPIRV-Tools-shared
    try $R -Wl,--start-group $A -Wl,--end-group
done
# 3. shared libraries (Fedora, Arch, openSUSE, newer Debian)
for extra in "" "-lSPIRV" "-lSPIRV-Tools-opt -lSPIRV-Tools" "-lSPIRV -lSPIRV-Tools-opt -lSPIRV-Tools" "-lSPIRV-Tools-shared"; do
    try -lglslang -lglslang-default-resource-limits $extra
done
try $(pkg-config --libs glslang 2>/dev/null) -lglslang-default-resource-limits
cat <<MSG
------------------------------------------------------------------------------------------------
libme1spv.so НЕ собрана: не удалось слинковаться с glslang (подробности: $LOG).
Это нужно только для Vulkan-рендерера. Игра работает и без него через OpenGL ES (это рендерер по умолчанию).
Чтобы включить Vulkan, установите пакеты и повторите make linux:
  Debian/Ubuntu: sudo apt install glslang-dev spirv-tools
  Fedora:        sudo dnf install glslang-devel spirv-tools-devel
  Arch:          sudo pacman -S glslang spirv-tools
  своя сборка:   make linux GLSLANG_LIBDIR=/путь/к/lib GLSLANG_CFLAGS=-I/путь/к/include
------------------------------------------------------------------------------------------------
MSG
exit 0
