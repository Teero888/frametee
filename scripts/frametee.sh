#!/bin/sh
export ASAN_OPTIONS=detect_odr_violation=0
export LSAN_OPTIONS=suppressions=asan_suppressions.txt
exec frametee "$@"
