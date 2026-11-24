#!/bin/sh
# Build mh2.exe: generated EE code (gen/) + runtime. Objects cached in build/.
set -e
cd "$(dirname "$0")"
CC=${CC:-clang}
OPT=${OPT:--O2}
mkdir -p build/obj
CFLAGS="$OPT -g -fno-strict-aliasing -fwrapv -Wall -Wno-unused-but-set-variable -Iruntime -Igen -Igen_vu -Wno-unused-function"
compile() {
  src=$1; obj=build/obj/$(echo "$src" | tr '/' '_' | sed 's/\.c$/.o/')
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ runtime/ee.h -nt "$obj" ]; then
    $CC $CFLAGS -c "$src" -o "$obj" || exit 1
  fi
}
export -f compile 2>/dev/null || true
export CC CFLAGS
ls gen/*.c gen_iop/*.c gen_vu/*.c runtime/*.c | xargs -P 16 -I{} sh -c '
  src={}; obj=build/obj/$(echo "$src" | tr "/" "_" | sed "s/\.c$/.o/")
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] || [ runtime/ee.h -nt "$obj" ] || [ runtime/rt.h -nt "$obj" ] || [ runtime/iop.h -nt "$obj" ] || { case "$src" in runtime/*) [ runtime/config.h -nt "$obj" ] || [ runtime/gs.h -nt "$obj" ];; *) false;; esac; } || { [ "$src" = runtime/vu.c ] && [ -f gen_vu/vu1_rec.h ] && [ gen_vu/vu1_rec.h -nt "$obj" ]; } || { case "$src" in runtime/vu.c|gen_vu/*) [ runtime/vu_core.h -nt "$obj" ];; *) false;; esac; }; then
    $CC $CFLAGS -c "$src" -o "$obj" || exit 255
  fi'
# link the objects of the current sources only: a regen that makes fewer gen/ or
# gen_vu/ files leaves stale objects behind, and a glob would link them too
OBJS=$(ls gen/*.c gen_iop/*.c gen_vu/*.c runtime/*.c | tr '/' '_' | sed 's/\.c$/.o/; s|^|build/obj/|')
$CC -g $OBJS -o build/mh2.exe -Wl,--stack,67108864 -lwinmm -lgdi32 -luser32 -lopengl32
echo "built build/mh2.exe"
