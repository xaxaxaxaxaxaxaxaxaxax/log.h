#include "log.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <optional>
#include <variant>
#include <vector>

using Clock = std::chrono::high_resolution_clock;

struct NullSink {
    int saved_fd, null_fd;
    NullSink() : saved_fd{dup(STDERR_FILENO)}, null_fd{open("/dev/null", O_WRONLY)} {
        dup2(null_fd, STDERR_FILENO);
    }
    ~NullSink() { dup2(saved_fd, STDERR_FILENO); close(null_fd); close(saved_fd); }
};

template<typename F>
double bench(const char* name, int iters, F&& f) {
    for (int i = 0; i < 100; ++i) f();

    std::vector<double> times;
    times.reserve(10);
    for (int run = 0; run < 10; ++run) {
        auto start = Clock::now();
        for (int i = 0; i < iters; ++i) f();
        auto end = Clock::now();
        times.push_back(double(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()) / iters);
    }
    std::ranges::sort(times);
    std::printf("  %-42s %8.1f ns/op\n", name, times[5]);
    return times[5];
}

void section(const char* name) { std::printf("\n%s\n", name); }

int main() {
    NullSink sink;
    Log::setMinLevel(Log::Debug);
    Log::setTimestamps(false);
    Log::setColors(false);

    constexpr int N = 100000;

    std::printf("log.h benchmark (%d iters, p50 of 10 runs)\n", N);
    std::printf("%s\n", std::string(56, '-').c_str());

    section("Baseline");
    bench("empty message", N, [] { Log::info("Ctx", ""); });
    bench("short message", N, [] { Log::info("Ctx", "Hello world"); });
    bench("long message (64B)", N, [] {
        Log::info("Ctx", "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!!");
    });

    section("Logfmt fields");
    Log::setFormat(Log::Logfmt);
    bench("1 int field", N, [] { Log::info("Ctx", Log::kv("n", 200)); });
    bench("1 string field", N, [] { Log::info("Ctx", Log::kv("s", "hello")); });
    bench("1 float field", N, [] { Log::info("Ctx", Log::kv("f", 3.14159)); });
    bench("3 mixed fields", N, [] {
        Log::info("HTTP", Log::kv("method", "GET"), Log::kv("path", "/api"), Log::kv("status", 200));
    });
    bench("5 fields", N, [] {
        Log::info("HTTP", Log::kv("method", "GET"), Log::kv("path", "/api/users"),
            Log::kv("status", 200), Log::kv("ms", 3.14), Log::kv("bytes", 1024));
    });
    bench("large int (10^12)", N, [] { Log::info("Ctx", Log::kv("v", 1234567890123LL)); });
    bench("msg + 1 field", N, [] { Log::info("Ctx", "hello", Log::kv("n", 42)); });

    section("JSON fields");
    Log::setFormat(Log::JSON);
    bench("1 int field", N, [] { Log::info("Ctx", Log::kv("n", 200)); });
    bench("3 mixed fields", N, [] {
        Log::info("HTTP", Log::kv("method", "GET"), Log::kv("path", "/api"), Log::kv("status", 200));
    });
    bench("escape needed", N, [] { Log::info("Ctx", Log::kv("m", "hello \"world\"\ntest")); });
    bench("no escape (32B string)", N, [] {
        Log::info("Ctx", Log::kv("m", "abcdefghijklmnopqrstuvwxyz012345"));
    });

    section("Timestamps");
    Log::setTimestamps(true);
    Log::setFormat(Log::Logfmt);
    bench("logfmt + timestamp", N, [] { Log::info("Ctx", "msg"); });
    Log::setFormat(Log::JSON);
    bench("json + iso8601 timestamp", N, [] { Log::info("Ctx", "msg"); });
    Log::setTimestamps(false);

    section("Thread ID");
    Log::setFormat(Log::Logfmt);
    Log::setThreadId(true);
    bench("with thread ID", N, [] { Log::info("Ctx", "msg"); });
    Log::setThreadId(false);

    section("Scoped fields");
    {
        auto scope = Log::Scope(Log::kv("req", "abc-123"));
        bench("1 scoped + 1 inline", N, [] { Log::info("Ctx", Log::kv("n", 42)); });
    }

    section("Types");
    Log::setFormat(Log::Logfmt);
    std::vector<int> vec = {1, 2, 3, 4, 5};
    bench("vector<int> (5 elem)", N, [&] { Log::info("Ctx", "v=", vec); });
    std::optional<int> opt = 42;
    bench("optional<int>", N, [&] { Log::info("Ctx", "o=", opt); });
    std::variant<int, std::string> var = 42;
    bench("variant<int,string>", N, [&] { Log::info("Ctx", "v=", var); });
    auto tp = std::chrono::system_clock::now();
    bench("time_point", N, [&] { Log::info("Ctx", "t=", tp); });

    section("Filtering");
    Log::setContextFilter("Other");
    bench("filtered out (no-op)", N, [] { Log::info("Ctx", "msg"); });
    Log::clearContextFilter();
    bench("no filter (baseline)", N, [] { Log::info("Ctx", "msg"); });

    section("Sampling");
    bench("sample<100> (1-in-100)", N, [] {
        if (Log::sample<100>()) Log::info("Ctx", "hit");
    });

    section("Hexdump");
    unsigned char buf[64];
    for (int i = 0; i < 64; ++i) buf[i] = static_cast<unsigned char>(i);
    bench("hexdump 64B", N, [&] { Log::info("Ctx", Log::hexdump(buf, 64)); });

    std::printf("\n");
}
