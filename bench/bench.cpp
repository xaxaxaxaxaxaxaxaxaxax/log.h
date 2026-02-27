#include "log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fcntl.h>
#include <numbers>
#include <numeric>
#include <optional>
#include <print>
#include <string_view>
#include <variant>
#include <vector>

using Clock = std::chrono::high_resolution_clock;

struct NullSink {
    int savedFd, nullFd;
    NullSink() : savedFd{dup(STDERR_FILENO)}, nullFd{open("/dev/null", O_WRONLY)} { dup2(nullFd, STDERR_FILENO); }
    ~NullSink() {
        dup2(savedFd, STDERR_FILENO);
        close(nullFd);
        close(savedFd);
    }
};

namespace {

    template<typename F>
    double bench(const std::string_view name, const int iters, const F &f) {
        for (int i = 0; i < 100; ++i)
            f();

        std::vector<double> times;
        times.reserve(10);
        for (int run = 0; run < 10; ++run) {
            const auto start = Clock::now();
            for (int i = 0; i < iters; ++i)
                f();
            const auto end = Clock::now();
            times.push_back(
                static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count()) / iters);
        }
        std::ranges::sort(times);
        const double median = times[5];
        std::println("  {:42} {:8.1f} ns/op", name, median);
        return median;
    }

    void section(const std::string_view name) {
        std::println("\n{}", name);
    }

} // namespace

int main() {
    const NullSink sink;
    Log::setMinLevel(Log::Debug);
    Log::setTimestamps(false);
    Log::setColors(false);

    constexpr int n = 100000;

    std::println("log.h benchmark ({} iters, p50 of 10 runs)", n);
    std::println("{}", std::string(56, '-'));

    section("Baseline");
    bench("empty message", n, [] { Log::info("Ctx", ""); });
    bench("short message", n, [] { Log::info("Ctx", "Hello world"); });
    bench("long message (64B)", n,
          [] { Log::info("Ctx", "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!!"); });

    section("Logfmt fields");
    Log::setFormat(Log::Logfmt);
    bench("1 int field", n, [] { Log::info("Ctx", Log::kv("n", 200)); });
    bench("1 string field", n, [] { Log::info("Ctx", Log::kv("s", "hello")); });
    bench("1 float field", n, [] { Log::info("Ctx", Log::kv("f", std::numbers::pi)); });
    bench("3 mixed fields", n,
          [] { Log::info("HTTP", Log::kv("method", "GET"), Log::kv("path", "/api"), Log::kv("status", 200)); });
    bench("5 fields", n, [] {
        Log::info("HTTP", Log::kv("method", "GET"), Log::kv("path", "/api/users"), Log::kv("status", 200),
                  Log::kv("ms", 3.14), Log::kv("bytes", 1024));
    });
    bench("large int (10^12)", n, [] { Log::info("Ctx", Log::kv("v", 1234567890123LL)); });
    bench("msg + 1 field", n, [] { Log::info("Ctx", "hello", Log::kv("n", 42)); });

    section("JSON fields");
    Log::setFormat(Log::JSON);
    bench("1 int field", n, [] { Log::info("Ctx", Log::kv("n", 200)); });
    bench("3 mixed fields", n,
          [] { Log::info("HTTP", Log::kv("method", "GET"), Log::kv("path", "/api"), Log::kv("status", 200)); });
    bench("escape needed", n, [] { Log::info("Ctx", Log::kv("m", "hello \"world\"\ntest")); });
    bench("no escape (32B string)", n, [] { Log::info("Ctx", Log::kv("m", "abcdefghijklmnopqrstuvwxyz012345")); });

    section("Timestamps");
    Log::setTimestamps(true);
    Log::setFormat(Log::Logfmt);
    bench("logfmt + timestamp", n, [] { Log::info("Ctx", "msg"); });
    Log::setFormat(Log::JSON);
    bench("json + iso8601 timestamp", n, [] { Log::info("Ctx", "msg"); });
    Log::setTimestamps(false);

    section("Thread ID");
    Log::setFormat(Log::Logfmt);
    Log::setThreadId(true);
    bench("with thread ID", n, [] { Log::info("Ctx", "msg"); });
    Log::setThreadId(false);

    section("Scoped fields");
    {
        auto scope = Log::Scope(Log::kv("req", "abc-123"));
        bench("1 scoped + 1 inline", n, [] { Log::info("Ctx", Log::kv("n", 42)); });
    }

    section("Types");
    Log::setFormat(Log::Logfmt);
    const std::vector vec = {1, 2, 3, 4, 5};
    bench("vector<int> (5 elem)", n, [&] { Log::info("Ctx", "v=", vec); });
    constexpr std::optional opt = 42;
    bench("optional<int>", n, [&] { Log::info("Ctx", "o=", opt); });
    constexpr std::variant<int, std::string_view> var = 42;
    bench("variant<int,string>", n, [&] { Log::info("Ctx", "v=", var); });
    const auto tp = std::chrono::system_clock::now();
    bench("time_point", n, [&] { Log::info("Ctx", "t=", tp); });

    section("Filtering");
    Log::setContextFilter("Other");
    bench("filtered out (no-op)", n, [] { Log::info("Ctx", "msg"); });
    Log::clearContextFilter();
    bench("no filter (baseline)", n, [] { Log::info("Ctx", "msg"); });

    section("Sampling");
    bench("sample<100> (1-in-100)", n, [] {
        if (Log::sample<100>()) Log::info("Ctx", "hit");
    });

    section("Hexdump");
    std::array<unsigned char, 64> buf{};
    std::ranges::iota(buf, static_cast<unsigned char>(0));
    bench("hexdump 64B", n, [&] { Log::info("Ctx", Log::hexdump(buf.data(), buf.size())); });

    std::println("");
}
