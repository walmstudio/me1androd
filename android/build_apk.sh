#!/bin/sh
# Builds out/me1port-launcher.apk without Gradle: NDK clang -> libme1port.so, javac -> d8 -> classes.dex, aapt2 link, zipalign, apksigner.
set -e
cd "$(dirname "$0")"
R=..
SDK=${ANDROID_SDK:-/data/tools/android-sdk}; BT=${ANDROID_BT:-$SDK/build-tools/30.0.3}; AJAR=${ANDROID_JAR:-$SDK/platforms/android-29/android.jar}
NDK=${NDK:-/data/tools/android-ndk-r27c}; CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang
JAVA_HOME=${JAVA_HOME:-/usr/lib/jvm/java-17-amazon-corretto.x86_64}; export JAVA_HOME PATH=$JAVA_HOME/bin:$PATH
B=build; rm -rf $B; mkdir -p $B/classes $B/lib/arm64-v8a $B/res $B/assets out
echo "[1/6] libme1port.so (arm64-v8a)"
$CC -O2 -shared -fPIC -fsigned-char -fwrapv -fno-strict-aliasing -w -Wl,-soname,libme1port.so -Wl,--gc-sections -Wl,-z,max-page-size=16384 -s \
    -I$R/runtime -I$R/runtime/khr -I$R/settings -o $B/lib/arm64-v8a/libme1port.so \
    jni/me1port_jni.c $R/runtime/rt.c $R/runtime/rt_fp.c $R/runtime/hle_win32.c $R/runtime/hle_sys.c $R/runtime/hle_fs.c $R/runtime/hle_mod.c $R/runtime/hle_thread.c $R/runtime/hle_seh.c $R/runtime/hle_exe.c $R/runtime/hle_vmem.c $R/runtime/hle_d3d9.c $R/runtime/hle_d3d9_cmd.c $R/runtime/gearmass.c $R/runtime/sm3_glsl.c $R/runtime/hle_hb.c $R/runtime/hle_input.c $R/runtime/hle_al.c \
    $R/settings/ini.c $R/settings/me1settings.c -lm -ldl
# v0.9.8: libme1vkshim.so - minimal Vulkan loader over one ICD (custom drivers loaded without libadrenotools)
$CC -O2 -shared -fPIC -fvisibility=hidden -Wl,-soname,libme1vkshim.so -Wl,-z,max-page-size=16384 -s -o $B/lib/arm64-v8a/libme1vkshim.so jni/vkshim.c
# the game (v0.8+): libme1game.so = recompiled MassEffect.exe + msvcr80/msvcp80/PhysX/D3DX + runtime + GLES backend,
# loaded by GameActivity; libme1run.so = small executable for the windowless test (Android allows exec() only from
# nativeLibraryDir, extractNativeLibs=true). Built by `make game-lib` in the parent folder. NO_EXE=1 skips both.
GAME=$R/build/android/libme1game.so; RUN=$R/build/android/libme1run.so
if [ -z "$NO_EXE" ] && [ -f $GAME ] && [ -f $RUN ]; then
    echo "[1b/6] libme1game.so, libme1run.so"
    $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip -o $B/lib/arm64-v8a/libme1game.so $GAME
    $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip -o $B/lib/arm64-v8a/libme1run.so $RUN
fi
# v0.10: libme1spv.so (GLSL -> SPIR-V for the Vulkan renderer; `make build/android/libme1spv.so`)
if [ -f $R/build/android/libme1spv.so ]; then cp $R/build/android/libme1spv.so $B/lib/arm64-v8a/; echo "      + libme1spv.so"; fi
# v0.9.8: optional GPU-driver libraries (gpu/build_gpu.sh -> ../build/android/gpu): Mesa zink (libEGL_mesa, libGLESv2_mesa,
# libgallium-*) + libadrenotools and its hook libraries (custom Vulkan drivers from GameExDriver). NO_GPU=1 skips them.
GPU=$R/build/android/gpu
if [ -z "$NO_GPU" ] && [ -f $GPU/libgallium-26.2.6.so ]; then
    echo "[1c/6] GPU drivers: $(cd $GPU && ls *.so | tr '\n' ' ')"
    cp $GPU/*.so $B/lib/arm64-v8a/
fi
echo "[2/6] javac"
javac -encoding UTF-8 -source 8 -target 8 -bootclasspath $AJAR:$BT/core-lambda-stubs.jar -Xlint:-options -d $B/classes $(find src -name '*.java')
echo "[3/6] d8"
$BT/d8 --release --min-api 29 --lib $AJAR --output $B $(find $B/classes -name '*.class')
echo "[4/6] aapt2"
$BT/aapt2 compile --dir res -o $B/res.zip
# assets: d3dx9_31.dll (data image for the recompiled D3DX in libme1exe.so); D3DX_DLL=path overrides, NO_D3DX=1 skips
D3DX_DLL=${D3DX_DLL:-/data/me1/game/dater/d3dx9_31.dll}
mkdir -p $B/assets
if [ -z "$NO_D3DX" ] && [ -f "$D3DX_DLL" ]; then cp "$D3DX_DLL" $B/assets/d3dx9_31.dll; echo "      + assets/d3dx9_31.dll"; fi
if [ -d assets ]; then cp -r assets/. $B/assets/; echo "      + assets/: $(ls assets | tr '\n' ' ')"; fi   # loading.jpg (start-up picture)
$BT/aapt2 link -o $B/base.apk -I $AJAR --manifest AndroidManifest.xml -A $B/assets $B/res.zip
echo "[5/6] add dex + native lib"
(cd $B && zip -q -X base.apk classes.dex && zip -q -X -r base.apk lib)
echo "[6/6] zipalign + sign"
$BT/zipalign -f -p 4 $B/base.apk $B/aligned.apk
KS=${KEYSTORE:-$(pwd)/keystore/me1port-debug.keystore}   # kept in the repo: same signature -> updates install over the old version
if [ ! -f "$KS" ]; then mkdir -p "$(dirname "$KS")"; keytool -genkeypair -keystore "$KS" -storepass android -keypass android -alias me1port \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=ME1 Port Debug" >/dev/null 2>&1; fi
$BT/apksigner sign --v1-signing-enabled true --v2-signing-enabled true --v3-signing-enabled true --ks "$KS" --ks-pass pass:android --key-pass pass:android --out out/me1port-launcher.apk $B/aligned.apk
$BT/apksigner verify --print-certs out/me1port-launcher.apk | head -1
ls -la out/me1port-launcher.apk
