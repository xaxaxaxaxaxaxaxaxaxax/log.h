# log.h ^_^

single-header c++23 structured logger. zero allocations, ~100ns/call. simd-accelerated when available. logs to stderr via raw `syscall` on x86-64 linux.

needs c++23 (gcc 14+ / clang 18+) and linux.

## demo

![demo output](demo.svg)

auto-detected if available: `expected`, `mdspan`, `stacktrace`, `std::format` fallback

## build

```
make run-example
make run-bench
./bench.sh
```

## config macros

- `LOG_LEVEL` (default 0) — compile-time min level, 0-3. below is compiled out.
- `LOG_MAX_SCOPED` (default 8) — max scoped fields per thread
- `LOG_MAX_FILTERS` (default 16) — max context filter entries
- `LOG_NO_GLOBAL_USING` — keep `Log` out of global namespace

## simd

uses sse2/ssse3/sse4.2/avx2 progressively for json escaping, hex encoding, digit packing, context filter hashing, and fast copies. all optional, scalar fallback always works.
