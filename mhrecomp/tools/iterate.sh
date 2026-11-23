#!/bin/sh
# Run the game; when it stops on an EE function the recompiler did not know,
# add the address to extra_ee.txt, regenerate, rebuild and run again.
# usage: sh tools/iterate.sh <rounds> [game args...]   (run from mhrecomp/)
rounds=$1; shift
for r in $(seq 1 "$rounds"); do
  rm -f ../disc/missing_entries.txt
  timeout ${SECS:-120} build/mh2.exe "$@" ../disc > ../run.log 2>&1
  code=$?
  echo "round $r: exit $code"
  [ -s ../disc/missing_entries.txt ] || break
  grep -q "^00[1-4]" ../disc/missing_entries.txt || { echo "stopped on a bad call target:"; cat ../disc/missing_entries.txt; break; }
  cat ../disc/missing_entries.txt | tee -a extra_ee.txt
  sh regen.sh || exit 1
  sh build.sh 2>&1 | grep -E "error|built"
done
tail -${TAIL:-8} ../run.log | cut -c1-200
