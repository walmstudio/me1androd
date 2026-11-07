#!/bin/sh
# Experimental GCC instrumentation for the recompiled executable on Linux only.
# Separate object/output paths: never overwrite the ordinary non-PGO executable.
set -eu
cd "$(dirname "$0")/.."
phase=${1:-}
case "$phase" in train|use) ;; *) echo 'usage: tools/pgo-linux.sh train|use' >&2; exit 2;; esac
[ "$#" -eq 1 ] || { echo "No extra arguments; use PGO_JOBS for parallel make." >&2; exit 2; }
command -v gcc >/dev/null || { echo 'GCC required' >&2; exit 1; }
[ -f out/exe/module.c ] || { echo 'Generate out/exe from your game first.' >&2; exit 1; }
mkdir -p build/pgo
manifest() { gcc --version | head -1; uname -m; find out/exe -maxdepth 1 -type f \( -name '*.c' -o -name '*.h' \) -print0 | sort -z | xargs -0 sha256sum; sha256sum runtime/rt.h; printf 'EXEOPT=%s\n' "${EXEOPT:--O1}"; }
if [ "$phase" = train ]; then
    rm -rf build/pgo/profiles build/pgo/exe build/pgo/rt
    mkdir -p build/pgo/profiles
    manifest > build/pgo/source.manifest
else
    [ -f build/pgo/source.manifest ] || { echo 'Run the training build first.' >&2; exit 1; }
    manifest > build/pgo/source.current
    cmp -s build/pgo/source.manifest build/pgo/source.current || { echo 'Compiler, architecture or generated source changed; retrain.' >&2; exit 1; }
    find build/pgo/profiles -name '*.gcda' -print -quit | grep -q . || { echo 'No profile data; run the training game and exit normally first.' >&2; exit 1; }
    rm -rf build/pgo/exe build/pgo/rt
fi
rm -f build/linux/me1-linux-pgo
# The output/object path is deliberately identical in both phases: GCC names
# .gcda files after the object path. All other objects remain non-instrumented.
make -j"${PGO_JOBS:-1}" CC=gcc "PGO_PHASE=$phase" "EXEOPT=${EXEOPT:--O1}" build/linux/me1-linux-pgo
echo "PGO $phase ready: build/linux/me1-linux-pgo"
