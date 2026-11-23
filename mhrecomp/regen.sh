#!/bin/sh
# Regenerate the EE C code. 42bf00-432980 holds the C++ static initialisers
# (called through the constructor table by fn_0037ee80), outside the main text.
cd "$(dirname "$0")"
python -I tools/recomp_ee.py ../disc/SLUS_203.35 gen --extra extra_ee.txt --code 42bf00-432980
