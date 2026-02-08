#!/bin/bash
set -e
CXX=${CXX:-g++}
echo "Compiler: $($CXX --version | head -1)"
echo

for flags in "-O2" "-O3" "-O3 -march=native" "-O3 -march=native -flto"; do
    echo "=== CXXFLAGS: $flags ==="
    $CXX -std=c++23 $flags -Wall -o bench_run bench.cpp 2>/dev/null
    ./bench_run
done

rm -f bench_run
