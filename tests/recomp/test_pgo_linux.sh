#!/bin/sh
# Smoke test of the GCC object-path/profile lifecycle and _exit flush.
set -eu
ROOT=$(pwd); T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/profiles" "$T/obj"
cat > "$T/example.c" <<'C'
#include <unistd.h>
extern void done(void);
volatile int sink;
int main(void) { for (int i=0; i<100; i++) sink += i; done(); _exit(0); }
C
cat > "$T/flush.c" <<'C'
#include "pgo.h"
void done(void) { me1_pgo_flush(); }
C
gcc -O1 -DME1_PGO_GCC -I"$ROOT/runtime" -c "$T/flush.c" -o "$T/flush.o"
gcc -O1 -I"$ROOT/runtime" -fprofile-generate="$T/profiles" -fprofile-update=atomic -c "$T/example.c" -o "$T/obj/example.o"
gcc -fprofile-generate="$T/profiles" "$T/obj/example.o" "$T/flush.o" -o "$T/train"
"$T/train"
find "$T/profiles" -name '*.gcda' -print -quit | grep -q .
rm "$T/obj/example.o"
gcc -O1 -I"$ROOT/runtime" -fprofile-use="$T/profiles" -fprofile-correction -Werror=missing-profile -c "$T/example.c" -o "$T/obj/example.o"
gcc -I"$ROOT/runtime" -c "$T/flush.c" -o "$T/flush.o"
gcc "$T/obj/example.o" "$T/flush.o" -o "$T/use"
"$T/use"
rm -rf "$T/profiles"; mkdir -p "$T/profiles"
if gcc -O1 -I"$ROOT/runtime" -fprofile-use="$T/profiles" -Werror=missing-profile -c "$T/example.c" -o "$T/obj/example.o" 2>/dev/null; then
  echo 'missing profile was not rejected' >&2; exit 1
fi
echo 'GCC PGO: manual flush, profile use, missing-profile guard OK'
