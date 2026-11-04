#!/bin/sh
# v0.10.13: regenerates out/ from the game's binaries (as used for the 0.10.13 APK).
#   tools/recomp_all.sh <folder with MassEffect.exe, PhysX*.dll, ogg/vorbis*.dll, binkw32.dll> <folder with msvcr80.dll, msvcp80.dll, d3dx9_31.dll>
# MassEffect.exe is incrementally linked: the jmp table at RVA 0x1005 (76236 thunks) hides the real function starts.
# tools/seeds/MassEffect_ilt_funcs.txt = the targets of those thunks; without them the C++ EH scan finds 459 FuncInfo
# instead of 12194 (and 4 landing targets instead of 22). Regenerate with tools/ilt_seeds.py for another build of the EXE.
set -e
cd "$(dirname "$0")/.."
G=${1:?game Binaries folder}; D=${2:-$1}
python3 tools/recomp.py "$D/msvcr80.dll" out/msvcr80 --prefix crt
python3 tools/recomp.py "$D/msvcp80.dll" out/msvcp80 --prefix cpp
python3 tools/recomp.py "$G/PhysXLoader.dll" out/physxloader --prefix pxl
python3 tools/recomp.py "$G/PhysXCore.dll" out/physxcore --prefix pxc --base 12000000
python3 tools/recomp.py "$G/ogg.dll" out/ogg --prefix ogg --base 10100000
python3 tools/recomp.py "$G/vorbis.dll" out/vorbis --prefix vb --base 10200000
python3 tools/recomp.py "$G/vorbisfile.dll" out/vorbisfile --prefix vf --base 10300000
python3 tools/recomp.py "$G/binkw32.dll" out/bink --prefix bk --base 10400000
python3 tools/recomp.py "$D/d3dx9_31.dll" out/d3dx9 --prefix dx --code-ranges 2b005-21e0a8
python3 tools/recomp.py "$G/MassEffect.exe" out/exe --prefix m --seeds tools/seeds/MassEffect_ilt_funcs.txt
