# log.h

Single-header C++23 structured logging library. No allocations, ~110ns per log call.
Formats integers, floats, containers, optionals, variants, durations, error codes, pointers, hex dumps, etc.
Uses SIMD (SSE2 through AVX2) for JSON escaping, hex encoding, and digit formatting when available, scalar fallback otherwise.

```cpp
#include "log.h"

Log::info("App", "starting");
Log::info("HTTP", "req", Log::kv("status", 200), Log::kv("ms", 3.14));
```

Needs C++23 (GCC 13+ / Clang 17+) and Linux/POSIX.

## build

```
make              # example + bench
make run-example
make run-bench
./bench.sh        # compare -O2 / -O3 / -march=native / -flto
```

## config macros

`LOG_LEVEL` (default 0) -- compile-time minimum level, 0=debug 1=info 2=warn 3=error. Anything below gets compiled out entirely.

`LOG_MAX_SCOPED` (default 8) -- max scoped fields per thread.

`LOG_MAX_FILTERS` (default 16) -- max context filter entries.

`LOG_NO_GLOBAL_USING` -- define this if you don't want `Log` in the global namespace (it lives in `logging::Log`).

## usage

```cpp
// levels
Log::debug("Ctx", ...);
Log::info("Ctx", ...);
Log::warning("Ctx", ...);
Log::error("Ctx", ...);

// structured fields
Log::info("Ctx", "msg", Log::kv("key", value));

// scoped fields -- attached to every log call until the scope ends
{ auto s = Log::Scope(Log::kv("req", id)); }

// timer -- logs elapsed on destruction
{ auto t = Log::Timer("Ctx", "operation"); }

// runtime config
Log::setFormat(Log::JSON);       // default is Log::Logfmt
Log::setMinLevel(Log::Warning);
Log::setTimestamps(true);
Log::setColors(true);            // or Log::autoDetectColors()
Log::setThreadId(true);
Log::setContextFilter("App,HTTP,-DB");

// sampling, rate limiting
if (Log::sample<100>()) Log::debug("Hot", "1-in-100");
LOG_RATE_LIMITED_MS(info, 1000, "Ctx", "at most once per second");

// conditional macros
LOG_ONCE(info, "Ctx", "first time only");
LOG_FIRST_N(info, 5, "Ctx", "first 5");
LOG_EVERY_N(info, 100, "Ctx", "every 100th");
LOG_IF(warning, x > limit, "Ctx", "over limit");
LOG_ASSERT(ptr != nullptr, "Ctx", "null pointer");
```
