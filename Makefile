CC ?= gcc
# v1.1: native aarch64 Linux host (Raspberry Pi 5, Ampere, Asahi...): x86 code needs signed char; RCPC (ARMv8.3+) gives
# cheap acquire loads for the TSO accesses. Raspberry Pi 4 / Cortex-A72: make ... ARM_FLAGS=-march=armv8-a
ifeq ($(shell uname -m),aarch64)
ARM_FLAGS ?= -march=armv8.2-a+rcpc
override CC := $(CC) -fsigned-char $(ARM_FLAGS)
endif
CFLAGS ?= -O1 -g -w -fwrapv -fno-strict-aliasing -Iruntime -Iruntime/khr -Isettings -Iout/physxloader

# v1.2.1: rebuild by content, not only by time. Sources unpacked from an archive keep the archive's timestamps, and
# those can be OLDER than the objects of the previous build (zip stores local time without a time zone) -> make kept
# stale runtime objects (libme1game.so: dlopen "cannot locate symbol hle_fs_read_bytes"). build/.srcsum remembers the
# md5 of every source file; a file whose content changed since the last make gets its mtime set to now, so the normal
# rules rebuild everything that depends on it. First run (no build/.srcsum yet) with an old build/: the runtime and
# launcher objects are deleted once (cheap; the recompiled game objects only depend on out/ and runtime/rt.h).
# SRCSUM=0 disables it.
ifneq ($(SRCSUM),0)
SRCSUM_DIRS = runtime settings linux android/jni android/assets/i18n tools
$(shell mkdir -p build; \
  find $(SRCSUM_DIRS) -type f \( -name '*.c' -o -name '*.h' -o -name '*.S' -o -name '*.map' -o -name '*.json' -o -name '*.py' -o -name '*.sh' \) \
    -not -path '*/__pycache__/*' -print0 2>/dev/null | sort -z | xargs -0 md5sum > build/.srcsum.new 2>/dev/null; \
  if [ -f build/.srcsum ]; then \
    grep -vxFf build/.srcsum build/.srcsum.new | cut -c35- | while IFS= read -r f; do [ -f "$$f" ] && touch -- "$$f"; done; \
  elif [ -d build/android/pic/rt ] || [ -d build/linux/rt ]; then \
    rm -f build/android/pic/rt/*.o build/linux/rt/*.o build/me1cfg build/android/me1cfg build/android/libme1run.so; \
  fi; \
  mv -f build/.srcsum.new build/.srcsum)
endif
RT = runtime/rt.c runtime/rt_fp.c runtime/hle_win32.c runtime/hle_sys.c runtime/hle_fs.c runtime/hle_mod.c runtime/hle_thread.c runtime/hle_seh.c runtime/hle_exe.c runtime/hle_vmem.c runtime/hle_d3d9.c runtime/hle_d3d9_cmd.c runtime/gearmass.c runtime/sm3_glsl.c runtime/hle_hb.c runtime/hle_input.c runtime/hle_al.c settings/ini.c settings/me1settings.c
PXL = $(wildcard out/physxloader/*.c)
build/test_pxl: $(RT) $(PXL) runtime/main_pxl.c runtime/*.h settings/*.h
	mkdir -p build && $(CC) $(CFLAGS) -o $@ $(RT) $(PXL) runtime/main_pxl.c -lm -ldl -pthread

# PhysXLoader + recompiled PhysXCore (generate first: python3 tools/recomp.py PhysXCore.dll out/physxcore --prefix pxc --base 12000000)
PXC = $(wildcard out/physxcore/*.c)
PXC_O = $(patsubst out/physxcore/%.c,build/pxc/%.o,$(PXC))
build/pxc/%.o: out/physxcore/%.c runtime/rt.h
	mkdir -p build/pxc && $(CC) -O1 -w -fwrapv -fno-strict-aliasing -Iruntime -Iout/physxcore -c $< -o $@
build/test_pxl_pxc: $(RT) $(PXL) $(PXC_O) runtime/main_pxl.c runtime/*.h settings/*.h
	mkdir -p build && $(CC) $(CFLAGS) -DWITH_PXC -o $@ $(RT) $(PXL) runtime/main_pxl.c $(PXC_O) -lm -ldl -pthread

# Android arm64 (NDK r27c, API 29 = Android 10)
NDK ?= /data/tools/android-ndk-r27c
ACC = $(NDK)/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android29-clang
AFLAGS = -O2 -march=armv8.2-a+rcpc -fsigned-char -fwrapv -fno-strict-aliasing -w -Iruntime -Iruntime/khr -Isettings -Iout/physxloader
build/android/test_pxl: $(RT) $(PXL) runtime/main_pxl.c runtime/*.h settings/*.h
	mkdir -p build/android && $(ACC) $(AFLAGS) -o $@ $(RT) $(PXL) runtime/main_pxl.c -lm -ldl -pthread
build/android/test_pxl_static: $(RT) $(PXL) runtime/main_pxl.c runtime/*.h settings/*.h
	mkdir -p build/android && $(ACC) $(AFLAGS) -static -o $@ $(RT) $(PXL) runtime/main_pxl.c -lm -ldl -pthread

# differential tests vs Unicorn (x86 host, then ARM64 under qemu-aarch64-static)
difftest:
	python3 tools/difftest.py --n 200 --seed 11
difftest-arm64:
	python3 tools/difftest.py --n 200 --seed 11 --work /tmp/dt_arm --cc "$(ACC) -static -fsigned-char" --run qemu-aarch64-static
.PHONY: difftest difftest-arm64

# Android arm64: PhysXLoader + recompiled PhysXCore, static (runs via adb shell or qemu-aarch64-static)
APXC_O = $(patsubst out/physxcore/%.c,build/android/pxc/%.o,$(PXC))
build/android/pxc/%.o: out/physxcore/%.c runtime/rt.h
	mkdir -p build/android/pxc && $(ACC) -O1 -march=armv8.2-a+rcpc -fsigned-char -fwrapv -fno-strict-aliasing -w -Iruntime -Iout/physxcore -c $< -o $@
build/android/test_pxl_pxc_static: $(RT) $(PXL) $(APXC_O) runtime/main_pxl.c runtime/*.h settings/*.h
	mkdir -p build/android && $(ACC) $(AFLAGS) -static -DWITH_PXC -o $@ $(RT) $(PXL) runtime/main_pxl.c $(APXC_O) -lm -ldl -pthread

# settings: me1cfg CLI (replacement for MassEffectConfig.exe) and VFS/Win32 file-layer test
SET = settings/ini.c settings/me1settings.c
build/me1cfg: $(SET) settings/me1cfg_cli.c settings/*.h
	mkdir -p build && $(CC) -O2 -Wall -o $@ $(SET) settings/me1cfg_cli.c
build/android/me1cfg: $(SET) settings/me1cfg_cli.c settings/*.h
	mkdir -p build/android && $(ACC) -O2 -fsigned-char -static -o $@ $(SET) settings/me1cfg_cli.c
build/test_fs: $(RT) runtime/test_fs.c runtime/*.h settings/*.h
	mkdir -p build && $(CC) $(CFLAGS) -o $@ $(RT) runtime/test_fs.c -lm -ldl -pthread
build/android/test_fs_static: $(RT) runtime/test_fs.c runtime/*.h settings/*.h
	mkdir -p build/android && $(ACC) $(AFLAGS) -static -o $@ $(RT) runtime/test_fs.c -lm -ldl -pthread

# launcher self-check as CLI (same code as the APK's "Проверить, что увидит игра") and the APK itself
JDK ?= /usr/lib/jvm/java-17-amazon-corretto.x86_64
CHK = android/jni/me1check_main.c android/jni/me1port_jni.c runtime/rt.c runtime/rt_fp.c runtime/hle_win32.c runtime/hle_sys.c runtime/hle_fs.c runtime/hle_mod.c runtime/hle_thread.c runtime/hle_seh.c runtime/hle_exe.c runtime/hle_vmem.c runtime/hle_d3d9.c runtime/hle_d3d9_cmd.c runtime/gearmass.c runtime/sm3_glsl.c runtime/hle_hb.c runtime/hle_input.c runtime/hle_al.c $(SET)
build/me1check: $(CHK) runtime/*.h settings/*.h
	mkdir -p build && $(CC) $(CFLAGS) -I$(JDK)/include -I$(JDK)/include/linux -o $@ $(CHK) -lm -ldl -pthread
build/android/me1check_static: $(CHK) runtime/*.h settings/*.h
	mkdir -p build/android && $(ACC) $(AFLAGS) -static -o $@ $(CHK) -lm -ldl -pthread
apk: build/android/libme1spv.so
	android/build_apk.sh
.PHONY: apk

# guest threads + sync: tiny Keystone-built DLL, recompiled, run with N threads (x86-64 and arm64/qemu)
out/mt_test/module.c: tests/mt/mk_mt_test.py tools/recomp.py
	python3 tests/mt/mk_mt_test.py build/mt_test.dll && python3 tools/recomp.py build/mt_test.dll out/mt_test --prefix mtt
build/test_mt: $(RT) out/mt_test/module.c runtime/main_mt.c runtime/*.h
	mkdir -p build && $(CC) $(CFLAGS) -Iout/mt_test -o $@ $(RT) out/mt_test/*.c runtime/main_mt.c -lm -ldl -pthread
build/android/test_mt_static: $(RT) out/mt_test/module.c runtime/main_mt.c runtime/*.h
	mkdir -p build/android && $(ACC) $(AFLAGS) -Iout/mt_test -static -o $@ $(RT) out/mt_test/*.c runtime/main_mt.c -lm -ldl -pthread
test-mt: build/test_mt build/android/test_mt_static
	./build/test_mt out/mt_test/image.bin 8 20000
	qemu-aarch64-static ./build/android/test_mt_static out/mt_test/image.bin 8 20000
.PHONY: test-mt

# exceptions: recompiled msvcr80.dll (SEH/C++ EH runtime of the game) + Keystone-built eh_test.dll
#   python3 tools/recomp.py /path/msvcr80.dll out/msvcr80 --prefix crt
CRT = $(wildcard out/msvcr80/*.c)
CRT_O = $(patsubst out/msvcr80/%.c,build/crt/%.o,$(CRT))
ACRT_O = $(patsubst out/msvcr80/%.c,build/android/crt/%.o,$(CRT))
build/crt/%.o: out/msvcr80/%.c runtime/rt.h
	mkdir -p build/crt && $(CC) -O1 -w -fwrapv -fno-strict-aliasing -Iruntime -Iout/msvcr80 -c $< -o $@
build/android/crt/%.o: out/msvcr80/%.c runtime/rt.h
	mkdir -p build/android/crt && $(ACC) -O1 -march=armv8.2-a+rcpc -fsigned-char -fwrapv -fno-strict-aliasing -w -Iruntime -Iout/msvcr80 -c $< -o $@
out/eh_test/module.c: tests/eh/mk_eh_test.py tools/recomp.py tools/ehscan.py
	python3 tests/eh/mk_eh_test.py build/eh_test.dll && python3 tools/recomp.py build/eh_test.dll out/eh_test --prefix eht
build/test_eh: $(RT) $(CRT_O) out/eh_test/module.c runtime/main_eh.c runtime/*.h
	mkdir -p build && $(CC) $(CFLAGS) -Iout/eh_test -o $@ $(RT) out/eh_test/*.c $(CRT_O) runtime/main_eh.c -lm -ldl -pthread
build/android/test_eh_static: $(RT) $(ACRT_O) out/eh_test/module.c runtime/main_eh.c runtime/*.h
	mkdir -p build/android && $(ACC) $(AFLAGS) -Iout/eh_test -static -o $@ $(RT) out/eh_test/*.c $(ACRT_O) runtime/main_eh.c -lm -ldl -pthread
test-eh: build/test_eh build/android/test_eh_static
	./build/test_eh out/msvcr80/image.bin out/eh_test/image.bin 10000 4
	qemu-aarch64-static ./build/android/test_eh_static out/msvcr80/image.bin out/eh_test/image.bin 2000 4
.PHONY: test-eh

# Ogg Vorbis (v0.9.3): ogg.dll, vorbis.dll, vorbisfile.dll of the game (ISACT decodes its music/voice banks with them),
# rebased into the free gap between PhysXLoader and MassEffect.exe:
#   python3 tools/recomp.py ogg.dll out/ogg --prefix ogg --base 10100000
#   python3 tools/recomp.py vorbis.dll out/vorbis --prefix vb --base 10200000
#   python3 tools/recomp.py vorbisfile.dll out/vorbisfile --prefix vf --base 10300000
VB_MODS = ogg vorbis vorbisfile
VB_C = $(foreach m,$(VB_MODS),$(wildcard out/$(m)/*.c))
VB_O = $(patsubst out/%.c,build/vb/%.o,$(VB_C))
AVB_O = $(patsubst out/%.c,build/android/vb/%.o,$(VB_C))
build/vb/%.o: out/%.c runtime/rt.h
	@mkdir -p $(dir $@) && $(CC) -O2 -w -fwrapv -fno-strict-aliasing -Iruntime -I$(dir $<) -c $< -o $@
build/android/vb/%.o: out/%.c runtime/rt.h
	@mkdir -p $(dir $@) && $(ACC) -O2 -fsigned-char -fwrapv -fno-strict-aliasing -w -Iruntime -I$(dir $<) -c $< -o $@
build/test_vorbis: $(RT) $(CRT_O) $(VB_O) runtime/test_vorbis.c runtime/*.h settings/*.h
	mkdir -p build && $(CC) $(CFLAGS) -o $@ $(RT) runtime/test_vorbis.c $(CRT_O) $(VB_O) -lm -ldl -pthread
# test data: ffmpeg (libvorbis) encodes test tones, the reference PCM is ffmpeg's libvorbis decode (same float->s16 rounding)
VBD = build/vorbis_data
$(VBD)/ok:
	mkdir -p $(VBD)
	ffmpeg -v error -y -f lavfi -i "aevalsrc='0.5*sin(2*PI*(200+900*t)*t)+0.05*(random(0)-0.5)|0.3*sin(2*PI*660*t)*sin(2*PI*0.7*t)':s=44100:d=4" -c:a libvorbis -q:a 4 -metadata title="ME1 test tone" $(VBD)/stereo44.ogg
	ffmpeg -v error -y -f lavfi -i "aevalsrc='0.6*sin(2*PI*300*t)*sin(2*PI*2*t)':s=22050:d=2.5" -c:a libvorbis -q:a 2 -metadata title="ME1 test tone" $(VBD)/mono22.ogg
	ffmpeg -v error -y -f lavfi -i "aevalsrc='0.9*sin(2*PI*(50+5000*t)*t)|0.2*(random(1)-0.5)':s=48000:d=3" -c:a libvorbis -q:a 0 -metadata title="ME1 test tone" $(VBD)/q0_48k.ogg
	for f in stereo44 mono22 q0_48k; do ffmpeg -v error -y -c:a libvorbis -i $(VBD)/$$f.ogg -f s16le -c:a pcm_s16le $(VBD)/$$f.raw; done
	echo "2 44100 176400" > $(VBD)/stereo44.txt; echo "1 22050 55125" > $(VBD)/mono22.txt; echo "2 48000 144000" > $(VBD)/q0_48k.txt
	head -c 20000 /dev/urandom > $(VBD)/notogg.ogg
	touch $@
test-vorbis: build/test_vorbis $(VBD)/ok
	./build/test_vorbis out/msvcr80/image.bin out $(VBD)
.PHONY: test-vorbis
build/android/test_vorbis_static: $(RT) $(ACRT_O) $(AVB_O) runtime/test_vorbis.c runtime/*.h settings/*.h
	mkdir -p build/android && $(ACC) $(AFLAGS) -static -o $@ $(RT) runtime/test_vorbis.c $(ACRT_O) $(AVB_O) -lm -ldl -pthread

# Bink video (v0.9.5): python3 tools/recomp.py binkw32.dll out/bink --prefix bk --base 10400000
BK_C = $(wildcard out/bink/*.c)
BK_O = $(patsubst out/bink/%.c,build/bk/%.o,$(BK_C))
build/bk/%.o: out/bink/%.c runtime/rt.h
	@mkdir -p build/bk && $(CC) -O2 -w -fwrapv -fno-strict-aliasing -Iruntime -Iout/bink -c $< -o $@
build/test_bink: $(RT) $(BK_O) runtime/test_bink.c runtime/*.h settings/*.h
	mkdir -p build && $(CC) $(CFLAGS) -o $@ $(RT) runtime/test_bink.c $(BK_O) -lm -ldl -pthread
BIKD = build/bink_data
BIK ?= /data/me1/game/dater/attractmovieloc.bik
$(BIKD)/ref.bgra:
	mkdir -p $(BIKD) && ln -sf $(BIK) $(BIKD)/movie.bik
	ffmpeg -v error -y -i $(BIKD)/movie.bik -frames:v 120 -f rawvideo -pix_fmt bgra $@
$(BIKD)/ref_audio.raw: $(BIKD)/ref.bgra
	ffmpeg -v error -y -i $(BIKD)/movie.bik -t 8 -ac 2 -ar 44100 -f s16le $@
test-bink: build/test_bink $(BIKD)/ref.bgra $(BIKD)/ref_audio.raw
	./build/test_bink out $(BIKD) movie.bik 120 $(BIKD)/ref.bgra $(BIKD) $(BIKD)/ref_audio.raw
.PHONY: test-bink

# MassEffect.exe: python3 tools/recomp.py /path/MassEffect.exe out/exe --prefix m --seeds ../analysis/ilt_funcs.txt
#                 python3 tools/recomp.py /path/msvcp80.dll out/msvcp80 --prefix cpp
CPP = $(wildcard out/msvcp80/*.c)
CPP_O = $(patsubst out/msvcp80/%.c,build/cpp/%.o,$(CPP))
ACPP_O = $(patsubst out/msvcp80/%.c,build/android/cpp/%.o,$(CPP))
EXE_C = $(wildcard out/exe/*.c)
EXE_O = $(patsubst out/exe/%.c,build/exe/%.o,$(EXE_C))
AEXE_O = $(patsubst out/exe/%.c,build/android/exe/%.o,$(EXE_C))
EXEOPT ?= -O1
build/cpp/%.o: out/msvcp80/%.c runtime/rt.h
	mkdir -p build/cpp && $(CC) -O1 -w -fwrapv -fno-strict-aliasing -Iruntime -Iout/msvcp80 -c $< -o $@
build/android/cpp/%.o: out/msvcp80/%.c runtime/rt.h
	mkdir -p build/android/cpp && $(ACC) -O1 -march=armv8.2-a+rcpc -fsigned-char -fwrapv -fno-strict-aliasing -w -Iruntime -Iout/msvcp80 -c $< -o $@
build/exe/%.o: out/exe/%.c runtime/rt.h
	mkdir -p build/exe && $(CC) $(EXEOPT) -w -fwrapv -fno-strict-aliasing -Iruntime -Iout/exe -c $< -o $@
build/android/exe/%.o: out/exe/%.c runtime/rt.h
	mkdir -p build/android/exe && $(ACC) $(EXEOPT) -march=armv8.2-a+rcpc -fsigned-char -fwrapv -fno-strict-aliasing -w -Iruntime -Iout/exe -c $< -o $@
# d3dx9_31.dll (April 2006 SDK, 9.15.779; its .rdata is merged into .text, hence --code-ranges):
#   python3 tools/recomp.py /path/d3dx9_31.dll out/d3dx9 --prefix dx --code-ranges 2b005-21e0a8
DX_C = $(wildcard out/d3dx9/*.c)
DX_O = $(patsubst out/d3dx9/%.c,build/dx/%.o,$(DX_C))
ADX_O = $(patsubst out/d3dx9/%.c,build/android/dx/%.o,$(DX_C))
build/dx/%.o: out/d3dx9/%.c runtime/rt.h
	mkdir -p build/dx && $(CC) -O2 -w -fwrapv -fno-strict-aliasing -Iruntime -Iout/d3dx9 -c $< -o $@
build/android/dx/%.o: out/d3dx9/%.c runtime/rt.h
	mkdir -p build/android/dx && $(ACC) -O2 -fsigned-char -fwrapv -fno-strict-aliasing -w -Iruntime -Iout/d3dx9 -c $< -o $@
build/test_exe: $(RT) $(CRT_O) $(CPP_O) $(PXL) $(PXC_O) $(DX_O) $(VB_O) $(BK_O) $(EXE_O) runtime/main_exe.c runtime/*.h
	mkdir -p build && $(CC) $(CFLAGS) -DWITH_PXC -DWITH_D3DX -DWITH_VORBIS -DWITH_BINK -o $@ $(RT) $(PXL) runtime/main_exe.c $(CRT_O) $(CPP_O) $(PXC_O) $(DX_O) $(VB_O) $(BK_O) $(EXE_O) -lm -ldl -pthread
build/android/test_exe_static: $(RT) $(ACRT_O) $(ACPP_O) $(PXL) $(APXC_O) $(ADX_O) $(AEXE_O) runtime/main_exe.c runtime/*.h
	mkdir -p build/android && $(ACC) $(AFLAGS) -static -DWITH_PXC -DWITH_D3DX -o $@ $(RT) $(PXL) runtime/main_exe.c $(ACRT_O) $(ACPP_O) $(APXC_O) $(ADX_O) $(AEXE_O) -lm -ldl -pthread
# recompiled D3DX: HLSL compiler/assembler/disassembler test (msvcr80 + d3dx9_31 images)
build/test_d3dx: $(RT) $(CRT_O) $(DX_O) runtime/test_d3dx.c runtime/*.h
	mkdir -p build && $(CC) $(CFLAGS) -o $@ $(RT) runtime/test_d3dx.c $(CRT_O) $(DX_O) -lm -ldl -pthread
build/android/test_d3dx_static: $(RT) $(ACRT_O) $(ADX_O) runtime/test_d3dx.c runtime/*.h
	mkdir -p build/android && $(ACC) $(AFLAGS) -static -o $@ $(RT) runtime/test_d3dx.c $(ACRT_O) $(ADX_O) -lm -ldl -pthread

# D3D9 HLE unit test (no game files needed)
build/test_hlsl_gl: $(RT) $(CRT_O) $(DX_O) runtime/test_hlsl_gl.c runtime/*.h
	mkdir -p build && $(CC) $(CFLAGS) -o $@ $(RT) runtime/test_hlsl_gl.c $(CRT_O) $(DX_O) -lm -ldl -pthread
build/test_gl: $(RT) runtime/test_gl.c runtime/*.h settings/*.h
	mkdir -p build && $(CC) $(CFLAGS) -o $@ $(RT) runtime/test_gl.c -lm -ldl -pthread

# v1.2.1: GEARMASS unit test (gearmass.c alone, runtime stubs)
build/test_gm: runtime/gearmass.c runtime/gearmass.h runtime/test_gm.c
	mkdir -p build && $(CC) -O1 -g -Wall -Wno-unused-parameter -Iruntime -o $@ runtime/gearmass.c runtime/test_gm.c -lm -pthread
build/test_cmd: $(RT) runtime/test_cmd.c runtime/*.h settings/*.h
	mkdir -p build && $(CC) $(CFLAGS) -o $@ $(RT) runtime/test_cmd.c -lm -ldl -pthread
# v0.9.8: one-ICD Vulkan "loader" for zink (custom drivers without libadrenotools); host copy for ME1_GPU_DRIVER=/path/icd.so tests
build/libme1vkshim.so: android/jni/vkshim.c
	mkdir -p build && $(CC) -O2 -shared -fPIC -fvisibility=hidden -o $@ $<
# v0.10: libme1spv.so - GLSL -> SPIR-V (glslang 15, static) for the Vulkan renderer (hle_d3d9_vk.h loads it with dlopen)
GLSLANG_SRC ?= /data/build3p/glslang-15.0.0
# host libme1spv.so: tools/build_spv_host.sh finds the system glslang (static on Debian/Ubuntu, shared on Fedora/Arch);
# if there is none, it only prints a hint (Vulkan renderer off, GL works) and the build goes on
build/libme1spv.so: android/jni/me1spv.c tools/build_spv_host.sh
	@CC="$(CC)" sh tools/build_spv_host.sh $@
GA = $(GLSLANG_SRC)/build-android/glslang
build/android/libme1spv.so: android/jni/me1spv.c android/jni/me1spv.map
	mkdir -p build/android && $(ACC) -O2 -fPIC -fvisibility=hidden -I$(GLSLANG_SRC) -I$(GLSLANG_SRC)/build-android/include -c -o build/android/me1spv.o $<
	$(ACC:clang=clang++) -shared -fPIC -Wl,-soname,libme1spv.so -Wl,-z,max-page-size=16384 -Wl,--gc-sections -Wl,--version-script=android/jni/me1spv.map -s \
	    -o $@ build/android/me1spv.o -Wl,--whole-archive $(GA)/libglslang-default-resource-limits.a -Wl,--no-whole-archive $(GA)/libglslang.a -static-libstdc++ -lm -Wl,--no-undefined
build/test_d3d9: $(RT) runtime/test_d3d9.c runtime/*.h settings/*.h
	mkdir -p build && $(CC) $(CFLAGS) -o $@ $(RT) runtime/test_d3d9.c -lm -ldl -pthread
build/android/test_d3d9_static: $(RT) runtime/test_d3d9.c runtime/*.h settings/*.h
	mkdir -p build/android && $(ACC) $(AFLAGS) -static -o $@ $(RT) runtime/test_d3d9.c -lm -ldl -pthread

# libme1game.so (Android, v0.8+): the whole game as one position-independent shared library, loaded by GameActivity
# (process ":game", window + GLES presenter) or by the launcher executable libme1run.so (test without a window).
# Everything is compiled with -fPIC: bionic refuses initial-exec TLS (rt_tcpu & co.) in dlopen'ed libraries.
PIC = build/android/pic
PEXE_O = $(patsubst out/exe/%.c,$(PIC)/exe/%.o,$(EXE_C))
PCRT_O = $(patsubst out/msvcr80/%.c,$(PIC)/crt/%.o,$(CRT))
PCPP_O = $(patsubst out/msvcp80/%.c,$(PIC)/cpp/%.o,$(CPP))
PDX_O = $(patsubst out/d3dx9/%.c,$(PIC)/dx/%.o,$(DX_C))
PPXC_O = $(patsubst out/physxcore/%.c,$(PIC)/pxc/%.o,$(PXC))
PVB_O = $(patsubst out/%.c,$(PIC)/vb/%.o,$(VB_C))
PBK_O = $(patsubst out/bink/%.c,$(PIC)/bk/%.o,$(BK_C))
PRT_SRC = $(RT) $(PXL) runtime/main_exe.c android/jni/game_jni.c
PRT_O = $(patsubst %.c,$(PIC)/rt/%.o,$(notdir $(PRT_SRC)))
vpath %.c runtime settings out/physxloader android/jni
$(PIC)/exe/%.o: out/exe/%.c runtime/rt.h
	@mkdir -p $(PIC)/exe && $(ACC) $(EXEOPT) -march=armv8.2-a+rcpc -fPIC -fsigned-char -fwrapv -fno-strict-aliasing -w -Iruntime -Iout/exe -c $< -o $@
$(PIC)/crt/%.o: out/msvcr80/%.c runtime/rt.h
	@mkdir -p $(PIC)/crt && $(ACC) -O1 -fPIC -fsigned-char -fwrapv -fno-strict-aliasing -w -Iruntime -Iout/msvcr80 -c $< -o $@
$(PIC)/cpp/%.o: out/msvcp80/%.c runtime/rt.h
	@mkdir -p $(PIC)/cpp && $(ACC) -O1 -fPIC -fsigned-char -fwrapv -fno-strict-aliasing -w -Iruntime -Iout/msvcp80 -c $< -o $@
$(PIC)/dx/%.o: out/d3dx9/%.c runtime/rt.h
	@mkdir -p $(PIC)/dx && $(ACC) -O2 -march=armv8.2-a+rcpc -fPIC -fsigned-char -fwrapv -fno-strict-aliasing -w -Iruntime -Iout/d3dx9 -c $< -o $@
$(PIC)/pxc/%.o: out/physxcore/%.c runtime/rt.h
	@mkdir -p $(PIC)/pxc && $(ACC) -O1 -fPIC -fsigned-char -fwrapv -fno-strict-aliasing -w -Iruntime -Iout/physxcore -c $< -o $@
$(PIC)/vb/%.o: out/%.c runtime/rt.h
	@mkdir -p $(dir $@) && $(ACC) -O2 -march=armv8.2-a+rcpc -fPIC -fsigned-char -fwrapv -fno-strict-aliasing -w -Iruntime -I$(dir $<) -c $< -o $@
$(PIC)/bk/%.o: out/bink/%.c runtime/rt.h
	@mkdir -p $(PIC)/bk && $(ACC) -O2 -march=armv8.2-a+rcpc -fPIC -fsigned-char -fwrapv -fno-strict-aliasing -w -Iruntime -Iout/bink -c $< -o $@
$(PIC)/rt/%.o: %.c runtime/*.h settings/*.h
	@mkdir -p $(PIC)/rt && $(ACC) $(AFLAGS) -fPIC -DME1_LIB -DWITH_PXC -DWITH_D3DX -DWITH_VORBIS -DWITH_BINK -c $< -o $@
build/android/libme1game.so: $(PRT_O) $(PCRT_O) $(PCPP_O) $(PPXC_O) $(PDX_O) $(PVB_O) $(PBK_O) $(PEXE_O) android/jni/game.map
	$(ACC) -shared -o $@ -Wl,-soname,libme1game.so -Wl,-Bsymbolic -Wl,--version-script=android/jni/game.map -Wl,--pack-dyn-relocs=android -Wl,-z,max-page-size=16384 \
	    $(PRT_O) $(PCRT_O) $(PCPP_O) $(PPXC_O) $(PDX_O) $(PVB_O) $(PBK_O) $(PEXE_O) -landroid -llog -lm -ldl
build/android/libme1run.so: android/jni/run_main.c
	$(ACC) -O2 -fPIE -pie -Wl,-z,max-page-size=16384 -o $@ $< -ldl
game-lib: build/android/libme1game.so build/android/libme1run.so
.PHONY: game-lib

# OpenAL HLE (hle_al.c) test: the mixer is driven by the test (ME1_AL_SINK=manual)
build/test_al: $(RT) runtime/test_al.c runtime/*.h settings/*.h
	mkdir -p build && $(CC) $(CFLAGS) -o $@ $(RT) runtime/test_al.c -lm -ldl -pthread
build/android/test_al_static: $(RT) runtime/test_al.c runtime/*.h settings/*.h
	mkdir -p build/android && $(ACC) $(AFLAGS) -static -o $@ $(RT) runtime/test_al.c -lm -ldl -pthread
# offline shader translator check on a dump of the game's shaders (ME1_D3D_DUMP=dir -> dir/shaders.pak)
build/sm3_check: runtime/sm3_check.c runtime/sm3_glsl.c runtime/sm3_glsl.h
	mkdir -p build && $(CC) -O2 -g -Wall -o $@ runtime/sm3_check.c runtime/sm3_glsl.c -lm

# PhoneModify engine modifiers (v0.10.4): parsing, caps, ini substitution, guest patches
build/test_mod: $(RT) runtime/test_mod.c runtime/*.h settings/*.h
	mkdir -p build && $(CC) $(CFLAGS) -o $@ $(RT) runtime/test_mod.c -lm -ldl -pthread
build/android/test_mod_static: $(RT) runtime/test_mod.c runtime/*.h settings/*.h
	mkdir -p build/android && $(ACC) $(AFLAGS) -static -o $@ $(RT) runtime/test_mod.c -lm -ldl -pthread

# ---- v1.1: desktop Linux (x86-64 / aarch64) - SDL2 front end linux/me1_linux.c + the same recompiled modules ----
# make linux       -> build/linux/me1-linux (+ build/linux/libme1spv.so, me1cfg); needs out/ from tools/recomp.py (see README)
# make linux-demo  -> build/linux/me1-demo: the front end with a D3D9/OpenAL/input demo instead of MassEffect.exe (no game files)
# aarch64 hosts: the same commands (CC gets -fsigned-char -march=armv8.2-a+rcpc, see the top; Raspberry Pi 4: ARM_FLAGS=-march=armv8-a)
SDL_CFLAGS ?= $(shell sdl2-config --cflags)
SDL_LIBS ?= $(shell sdl2-config --libs)
LCFLAGS = -O2 -g -fsigned-char -fwrapv -fno-strict-aliasing -w -Iruntime -Iruntime/khr -Isettings -Iout/physxloader
LRT_O = $(patsubst %.c,build/linux/rt/%.o,$(notdir $(RT) $(PXL)))
LDEF = -DME1_LIB -DWITH_PXC -DWITH_D3DX -DWITH_VORBIS -DWITH_BINK
build/linux/rt/%.o: %.c runtime/*.h settings/*.h
	@mkdir -p build/linux/rt && $(CC) $(LCFLAGS) -c $< -o $@
build/linux/rt/main_exe.o: runtime/main_exe.c runtime/*.h
	@mkdir -p build/linux/rt && $(CC) $(LCFLAGS) $(LDEF) -c $< -o $@
build/linux/rt/me1_linux.o: linux/me1_linux.c linux/*.h runtime/*.h settings/*.h
	@mkdir -p build/linux/rt && $(CC) $(LCFLAGS) -Ilinux -Wall -Wno-unused-parameter -Wno-misleading-indentation $(SDL_CFLAGS) -c $< -o $@
build/linux/rt/me1_linux_demo.o: linux/me1_linux.c linux/*.h runtime/*.h settings/*.h
	@mkdir -p build/linux/rt && $(CC) $(LCFLAGS) -Ilinux -DME1_DEMO_BUILD $(SDL_CFLAGS) -c $< -o $@
build/linux/rt/demo_d3d9.o: linux/demo_d3d9.c runtime/*.h
	@mkdir -p build/linux/rt && $(CC) $(LCFLAGS) -c $< -o $@
# v1.1.2: the launcher's UI (also the loading screen of me1-linux), translations, GPU probe, embedded fonts/images/json
LUI_O = build/linux/rt/ui.o build/linux/rt/i18n.o build/linux/rt/gpuprobe.o build/linux/rt/assets.o
LUI_FLAGS = -O2 -g -Wall -Wno-format-truncation -Wno-misleading-indentation -Wno-unused-function -Ilinux -Isettings -Iruntime/khr $(SDL_CFLAGS)
build/linux/rt/ui.o: linux/ui.c linux/ui.h linux/third_party/*.h
	@mkdir -p build/linux/rt && $(CC) $(LUI_FLAGS) -c $< -o $@
build/linux/rt/i18n.o: linux/i18n.c linux/i18n.h
	@mkdir -p build/linux/rt && $(CC) $(LUI_FLAGS) -c $< -o $@
build/linux/rt/gpuprobe.o: linux/gpuprobe.c linux/gpuprobe.h
	@mkdir -p build/linux/rt && $(CC) $(LUI_FLAGS) -c $< -o $@
build/linux/rt/assets.o: linux/assets.S linux/assets/* android/assets/loading.jpg android/assets/i18n/*.json linux/i18n/*.json
	@mkdir -p build/linux/rt && $(CC) -c $< -o $@
build/linux/rt/launcher.o: linux/launcher.c linux/*.h settings/*.h
	@mkdir -p build/linux/rt && $(CC) $(LUI_FLAGS) -Wno-unused-parameter -c $< -o $@
build/linux/rt/l_ini.o: settings/ini.c settings/ini.h
	@mkdir -p build/linux/rt && $(CC) $(LUI_FLAGS) -c $< -o $@
build/linux/rt/l_me1settings.o: settings/me1settings.c settings/*.h
	@mkdir -p build/linux/rt && $(CC) $(LUI_FLAGS) -w -c $< -o $@
build/linux/me1-launcher: build/linux/rt/launcher.o build/linux/rt/l_ini.o build/linux/rt/l_me1settings.o $(LUI_O)
	$(CC) -o $@ $^ $(SDL_LIBS) -lm -ldl -pthread
# the recompiled modules: the same host objects as build/test_exe (build/exe, build/crt, build/dx, ...)
linux-check:
	@test -n "$(EXE_C)" -a -n "$(CRT)" -a -n "$(CPP)" || { echo "make linux: нет out/exe, out/msvcr80, out/msvcp80 - сначала рекомпиляция своих файлов игры (linux/README.md, шаг 2)"; exit 1; }
.PHONY: linux-check
build/linux/me1-linux: | linux-check
build/linux/me1-linux: $(LRT_O) build/linux/rt/main_exe.o build/linux/rt/me1_linux.o $(LUI_O) $(CRT_O) $(CPP_O) $(PXC_O) $(DX_O) $(VB_O) $(BK_O) $(EXE_O)
	$(CC) -o $@ $^ $(SDL_LIBS) -lm -ldl -pthread -rdynamic
build/linux/me1-demo: $(LRT_O) build/linux/rt/demo_d3d9.o build/linux/rt/me1_linux_demo.o $(LUI_O)
	$(CC) -o $@ $^ $(SDL_LIBS) -lm -ldl -pthread -rdynamic
build/linux/libme1spv.so: android/jni/me1spv.c tools/build_spv_host.sh
	@CC="$(CC)" sh tools/build_spv_host.sh $@
build/linux/me1cfg: build/me1cfg
	@mkdir -p build/linux && cp $< $@
linux: build/linux/me1-linux build/linux/libme1spv.so build/linux/me1cfg build/linux/me1-launcher
linux-demo: build/linux/me1-demo build/linux/libme1spv.so build/linux/me1-launcher
linux-install: linux
	linux/install.sh
.PHONY: linux linux-demo linux-install
