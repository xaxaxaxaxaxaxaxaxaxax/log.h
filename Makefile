CXX      ?= g++
CXXFLAGS ?= -std=c++26 -O2 -march=native -Wall -Wextra

.PHONY: all clean check run-example run-bench

all: examples/example bench/bench

examples/example: examples/example.cpp log.h
	$(CXX) $(CXXFLAGS) -I. -o $@ $< -lpthread

bench/bench: bench/bench.cpp log.h
	$(CXX) $(CXXFLAGS) -O3 -I. -o $@ $<

check:
	@echo '#include "log.h"' | $(CXX) $(CXXFLAGS) -x c++ -c - -o /dev/null -fsyntax-only && echo "OK"

run-example: examples/example
	./examples/example

run-bench: bench/bench
	./bench/bench

clean:
	rm -f examples/example bench/bench bench/bench_run
