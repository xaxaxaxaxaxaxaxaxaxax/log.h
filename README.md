# log.h ^_^

single-header c++23 structured logger. zero allocations, ~100ns/call. simd-accelerated when available. logs to stderr via raw `syscall` on x86-64 linux.

needs c++23 (gcc 14+ / clang 18+) and linux.

## demo

```cpp
Log::info("App", "starting up");
Log::debug("App", "version=", 3, " build=", "release");
Log::warning("App", "disk usage high");
Log::error("App", "connection refused");
```
```ansi
[32m[INFO ][0m [App] starting up
[36m[DEBUG][0m [App] version=3 build=release
[33m[WARN ][0m [App] disk usage high
[31m[ERROR][0m [App] connection refused
```

```cpp
Log::info("HTTP", "request",
    Log::kv("method", "GET"), Log::kv("path", "/api/users"),
    Log::kv("status", 200), Log::kv("latency_ms", 3.14));
```
```ansi
[32m[INFO ][0m [HTTP] request method=GET path=/api/users status=200 latency_ms=3.14
```

```cpp
{
    auto scope = Log::Scope(Log::kv("req_id", "abc-123"), Log::kv("user", 42));
    Log::info("DB", "query", Log::kv("rows", 100));
    Log::info("Cache", "miss", Log::kv("key", "user:42"));
}
```
```ansi
[32m[INFO ][0m [DB] query req_id=abc-123 user=42 rows=100
[32m[INFO ][0m [Cache] miss req_id=abc-123 user=42 key=user:42
[32m[INFO ][0m [DB] insert req_id=abc-123 user=42 tx=write table=events
```

```cpp
{ auto timer = Log::Timer("App", "computation"); /* ... */ }
```
```ansi
[36m[DEBUG][0m [App] computation elapsed_us=185
```

```cpp
Log::info("Type", "bool: ", true, " ", false);
Log::info("Type", "int=", 42, " pi=", 3.14, " float=", 3.14f);
Log::info("Type", "ptr=", (void*)&x, " null=", nullptr);
Log::info("Type", "byte=", std::byte{0xAB});
Log::info("Type", "some=", std::optional{42}, " none=", std::optional<int>{});
Log::info("Type", "variant=", std::variant<int,std::string>{"hello"});
Log::info("Type", "shared=", std::make_shared<int>(99), " null=", std::unique_ptr<int>{});
Log::info("Type", "ec=", std::make_error_code(std::errc::no_such_file_or_directory));
Log::info("Type", "errc=", std::errc::invalid_argument);
Log::info("Type", "dur=", 150ms, " fast=", 42us, " nano=", 999ns);
Log::info("Type", "now=", std::chrono::system_clock::now());
Log::info("Type", "tid=", std::this_thread::get_id());
Log::info("Type", "status=", Status::OK);  // enum
Log::info("Type", "pair=", std::pair{42, "hello"});
Log::info("Type", "tuple=", std::tuple{1, 3.14, "x"});
Log::info("Type", "vec=", std::vector{1, 2, 3, 4, 5});
Log::info("Type", "map=", std::map<std::string,int>{{"a",1},{"b",2}});
Log::info("Type", "span=", std::span{bytes});
Log::info("Type", "path=", std::filesystem::path{"/tmp/test.txt"});
Log::info(Log::Loc{}, "Type", "with source location");
```
```ansi
[32m[INFO ][0m [Type] bool: true false
[32m[INFO ][0m [Type] int=42 uint=123 i64=-99
[32m[INFO ][0m [Type] pi=3.14159 e=2.71828 zero=0.0 neg=-1.5
[32m[INFO ][0m [Type] float=3.14
[32m[INFO ][0m [Type] cstr=hello
[32m[INFO ][0m [Type] sv=world
[32m[INFO ][0m [Type] str=std::string
[32m[INFO ][0m [Type] path=/tmp/test.txt
[32m[INFO ][0m [Type] ptr=0x7ffc4b3cbd80 null=null
[32m[INFO ][0m [Type] byte=ab
[32m[INFO ][0m [Type] null=null mono=null
[32m[INFO ][0m [Type] some=42 none=null
[32m[INFO ][0m [Type] variant=hello
[32m[INFO ][0m [Type] shared=0x10d765e0 null=null
[32m[INFO ][0m [Type] ec=generic:2
[32m[INFO ][0m [Type] errc=generic:22
[32m[INFO ][0m [Type] dur=150ms fast=42us nano=999ns sec=3s
[32m[INFO ][0m [Type] now=2026-02-09T19:55:40.686Z
[32m[INFO ][0m [Type] tid=tid:0x4579383a22116b50
[32m[INFO ][0m [Type] with source location ([2mexample.cpp:104[0m)
[32m[INFO ][0m [Type] status=0
[32m[INFO ][0m [Type] pair=(42, hello)
[32m[INFO ][0m [Type] tuple=(1, 3.14, x)
[32m[INFO ][0m [Type] vec=[1, 2, 3, 4, 5]
[32m[INFO ][0m [Type] map=[(a, 1), (b, 2)]
[32m[INFO ][0m [Type] arr=[10, 20, 30]
[32m[INFO ][0m [Type] span=[de, ad, be, ef]
```

```cpp
Log::info("Net", "packet:\n", Log::hexdump(pkt, sizeof(pkt)));
```
```ansi
[32m[INFO ][0m [Net] packet:
0000  45 00 00 3c 1c 46 40 00  40 06 b1 e6 ac 10 0a 63 |E..<.F@.@......c|
0010  ac 10 0a 0c 00 50 c0 1e                          |.....P..|
```

```cpp
Log::setFormat(Log::JSON);
Log::info("App", "json output",
    Log::kv("string", "hello \"world\""), Log::kv("num", 42),
    Log::kv("float", 3.14), Log::kv("flag", true));
```
```json
{"level":"info","ctx":"App","ts":"2026-02-09T19:55:40.686Z","msg":"json output","string":"hello \"world\"","num":42,"float":3.14,"flag":true}
```

```cpp
// sampling + rate limiting + conditional macros
if (Log::sample<100>()) Log::debug("Hot", "1-in-100");
LOG_RATE_LIMITED_MS(info, 1000, "Ctx", "once per second");
LOG_ONCE(info, "Ctx", "first time only");
LOG_FIRST_N(info, 5, "Ctx", "first 5");
LOG_EVERY_N(info, 100, "Ctx", "every 100th");
LOG_IF(warning, x > limit, "Ctx", "over limit");
LOG_ASSERT(ptr, "Ctx", "shouldn't be null");
LOG_DLOG("Ctx", "compiled out when LOG_LEVEL > 0");

// runtime config
Log::setMinLevel(Log::Warning);
Log::setTimestamps(true);
Log::setColors(true);              // or Log::autoDetectColors()
Log::setThreadId(true);
Log::setContextFilter("App,-DB");  // allow App, block DB
```

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
