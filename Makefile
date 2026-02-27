CXX      ?= g++
CXXFLAGS ?= -std=c++26 -O2 -march=native -Wall -Wextra

.PHONY: all clean check run-example run-bench

all: example bench

example: example.cpp log.h
	$(CXX) $(CXXFLAGS) -o $@ $< -lpthread

bench: bench.cpp log.h
	$(CXX) $(CXXFLAGS) -O3 -o $@ $<

check:
	@echo '#include "log.h"' | $(CXX) $(CXXFLAGS) -x c++ -c - -o /dev/null -fsyntax-only && echo "OK"

run-example: example
	./example

run-bench: bench
	./bench

clean:
	rm -f example bench
