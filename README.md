# log.h ^_^

single-header c++23 structured logger. zero allocations, ~100ns/call. simd-accelerated when available, scalar fallback otherwise. logs to stderr via raw `syscall` on x86-64 linux.

```cpp
#include "log.h"

Log::info("App", "starting");
Log::info("HTTP", "req", Log::kv("status", 200), Log::kv("ms", 3.14));
```

needs c++23 (gcc 14+ / clang 18+) and linux.

## build

```
make run-example
make run-bench
./bench.sh
```

## usage

```cpp
Log::debug("Ctx", ...);
Log::info("Ctx", ...);
Log::warning("Ctx", ...);
Log::error("Ctx", ...);

// structured fields
Log::info("Ctx", "msg", Log::kv("key", value));

// scoped fields — stick to every log call until scope ends
{ auto s = Log::Scope(Log::kv("req", id)); }

// timer — logs elapsed on destruction
{ auto t = Log::Timer("Ctx", "operation"); }

// source location
Log::info(Log::Loc{}, "Ctx", "includes file:line");

// runtime config
Log::setFormat(Log::JSON);
Log::setMinLevel(Log::Warning);
Log::setTimestamps(true);
Log::setColors(true);  // or Log::autoDetectColors()
Log::setThreadId(true);
Log::setContextFilter("App,HTTP,-DB");

// sampling + rate limiting
if (Log::sample<100>()) Log::debug("Hot", "1-in-100");
LOG_RATE_LIMITED_MS(info, 1000, "Ctx", "once per second");

// conditional macros
LOG_ONCE(info, "Ctx", "first time only");
LOG_FIRST_N(info, 5, "Ctx", "first 5");
LOG_EVERY_N(info, 100, "Ctx", "every 100th");
LOG_IF(warning, x > limit, "Ctx", "over limit");
LOG_ASSERT(ptr, "Ctx", "null pointer");
LOG_DLOG("Ctx", "compiled out when LOG_LEVEL > 0");
```

## types

everything formatted inline, no allocations:

| type | output |
|---|---|
| `bool` | `true` / `false` |
| integers | decimal, simd digit packing |
| floats | fixed-point when possible, `to_chars` fallback |
| `const char*`, `string`, `string_view`, `filesystem::path` | string content |
| `void*`, pointers | `0x...` hex |
| `byte` | 2-digit hex |
| `nullptr_t`, `monostate` | `null` |
| `optional<T>` | value or `null` |
| `variant<Ts...>` | active alternative |
| `shared_ptr<T>`, `unique_ptr<T>` | raw pointer or `null` |
| `error_code`, `errc` | `category:value` |
| `chrono::duration` | value + suffix (`ns`, `us`, `ms`, `s`, `min`, `h`, `d`) |
| `chrono::time_point` | iso 8601 timestamp |
| `thread::id` | `tid:0x...` |
| `source_location` | `file:line` |
| `pair`, `tuple` | `(a, b, ...)` |
| ranges / containers | `[a, b, ...]` (max 16) |
| `span<byte-like>` | `[hex bytes]` (max 32) |
| enums | underlying integer |
| `hexdump(ptr, len)` | multi-line hex + ascii dump |

auto-detected if available: `expected`, `mdspan`, `stacktrace`, `std::format` fallback

## output

logfmt (default):
```
[INFO ] [HTTP] req method=GET path=/api status=200 latency_ms=3.14
```

json:
```json
{"level":"info","ctx":"HTTP","msg":"req","method":"GET","path":"/api","status":200,"latency_ms":3.14}
```

## config macros

- `LOG_LEVEL` (default 0) — compile-time min level, 0-3. below is compiled out.
- `LOG_MAX_SCOPED` (default 8) — max scoped fields per thread
- `LOG_MAX_FILTERS` (default 16) — max context filter entries
- `LOG_NO_GLOBAL_USING` — keep `Log` out of global namespace

## simd

uses sse2/ssse3/sse4.2/avx2 progressively for json escaping, hex encoding, digit packing, context filter hashing, and fast copies. all optional, scalar fallback always works.
