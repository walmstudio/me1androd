#!/bin/sh
# v0.9.8 (v0.9.9: patch logs every silent zink failure, maintenance5 optional): builds the optional GPU-driver libraries of the APK into ../../build/android/gpu (build_apk.sh copies them):
#   Mesa 26.2.6 (only EGL + GLES2/3 + gallium zink, surfaceless EGL, no LLVM) -> libEGL_mesa.so, libGLESv2_mesa.so, libgallium-26.2.6.so
#   libadrenotools (bylaws, 8fae8ce, with linkernsbypass aa39758) -> libadrenotools.so + 4 hook libraries (lib*_hook.so, libhook_impl.so)
# Needs: NDK r27c, meson >= 1.4, ninja, cmake, python3 + mako/pyyaml/packaging, bison, flex, git, network (Mesa subproject libdrm, adrenotools).
set -e
HERE=$(cd "$(dirname "$0")" && pwd); OUT=$HERE/../../build/android/gpu
W=${W:-/data/build3p}; NDK=${NDK:-/data/tools/android-ndk-r27c}
mkdir -p "$W" "$OUT"; cd "$W"
# ---- Mesa
if [ ! -d mesa-26.2.6 ]; then
    [ -f mesa-26.2.6.tar.xz ] || curl -LO https://archive.mesa3d.org/mesa-26.2.6.tar.xz
    tar -xJf mesa-26.2.6.tar.xz
    (cd mesa-26.2.6 && patch -p1 < "$HERE/mesa-26.2.6-me1.patch")
fi
mkdir -p inc/cutils; cp mesa-26.2.6/include/android_stub/cutils/native_handle.h inc/cutils/   # util/u_gralloc wants it, Android build only
sed "s#/data/tools/android-ndk-r27c#$NDK#g" "$HERE/android-aarch64.cross" > android-aarch64.cross
cd mesa-26.2.6
[ -d build-android ] || meson setup build-android --cross-file ../android-aarch64.cross --buildtype=release \
    -Dplatforms= -Degl=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true -Dglx=disabled -Dgbm=disabled -Dglvnd=disabled \
    -Dgallium-drivers=zink -Dvulkan-drivers= -Dllvm=disabled -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled \
    -Dlmsensors=disabled -Dvalgrind=disabled -Dlibunwind=disabled -Degl-lib-suffix=_mesa -Dgles-lib-suffix=_mesa \
    -Dallow-fallback-for=libdrm --force-fallback-for=libdrm -Dlibdrm:default_library=static \
    -Dc_args="-O2 -fPIC -I$W/inc" -Dcpp_args="-O2 -fPIC -I$W/inc"
ninja -C build-android
S=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
$S -o "$OUT/libEGL_mesa.so" build-android/src/egl/libEGL_mesa.so
$S -o "$OUT/libGLESv2_mesa.so" build-android/src/mesa/glapi/es2api/libGLESv2_mesa.so
$S -o "$OUT/libgallium-26.2.6.so" build-android/src/gallium/targets/dri/libgallium-26.2.6.so
cd "$W"
# ---- libadrenotools
if [ ! -d libadrenotools ]; then
    git clone https://github.com/bylaws/libadrenotools.git && (cd libadrenotools && git checkout 8fae8ce254dfc1344527e05301e43f37dea2df80 && git submodule update --init)
fi
cd libadrenotools
[ -d build ] || cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384"
ninja -C build
$S -o "$OUT/libadrenotools.so" build/libadrenotools.so
for h in libmain_hook libhook_impl libfile_redirect_hook libgsl_alloc_hook; do $S -o "$OUT/$h.so" build/src/hook/$h.so; done
ls -la "$OUT"
