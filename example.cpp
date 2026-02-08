#include "log.h"
#include <map>
#include <memory>
#include <optional>
#include <thread>
#include <variant>
#include <vector>

enum class Status { OK, Error, Timeout };

int main() {
    Log::autoDetectColors();
    Log::setTimestamps(true);

    Log::info("App", "starting up");
    Log::debug("App", "version=", 3, " build=", "release");
    Log::warning("App", "disk usage high");
    Log::error("App", "connection refused");

    Log::info("HTTP", "request",
        Log::kv("method", "GET"),
        Log::kv("path", "/api/users"),
        Log::kv("status", 200),
        Log::kv("latency_ms", 3.14));

    {
        auto scope = Log::Scope(Log::kv("req_id", "abc-123"), Log::kv("user", 42));
        Log::info("DB", "query", Log::kv("rows", 100));
        Log::info("Cache", "miss", Log::kv("key", "user:42"));
        {
            auto inner = Log::Scope(Log::kv("tx", "write"));
            Log::info("DB", "insert", Log::kv("table", "events"));
        }
    }

    {
        auto timer = Log::Timer("App", "computation");
        volatile int sum = 0;
        for (int i = 0; i < 1000000; ++i) sum += i;
    }

    std::vector<int> v = {1, 2, 3, 4, 5};
    Log::info("App", "vec=", v);
    std::map<std::string, int> m = {{"a", 1}, {"b", 2}};
    Log::info("App", "map=", m);
    Log::info("App", "pair=", std::pair{42, "hello"});
    Log::info("App", "tuple=", std::tuple{1, 3.14, "x"});

    std::optional<int> some = 42;
    std::optional<int> none;
    Log::info("App", "some=", some, " none=", none);

    std::variant<int, std::string> var = "hello";
    Log::info("App", "variant=", var);

    auto sp = std::make_shared<int>(99);
    std::unique_ptr<int> np;
    Log::info("App", "shared=", sp, " null=", np);

    auto ec = std::make_error_code(std::errc::no_such_file_or_directory);
    Log::info("App", "err=", ec);

    using namespace std::chrono_literals;
    Log::info("App", "dur=", 150ms, " fast=", 42us);
    Log::info("App", "status=", Status::OK);
    Log::info("App", "pi=", 3.14159, " e=", 2.71828, " zero=", 0.0, " neg=", -1.5);

    Log::info(Log::Loc{}, "App", "with source location");

    Log::setFormat(Log::JSON);
    Log::info("App", "json output",
        Log::kv("string", "hello \"world\""),
        Log::kv("num", 42),
        Log::kv("float", 3.14),
        Log::kv("flag", true));
    Log::setFormat(Log::Logfmt);

    const unsigned char pkt[] = {
        0x45, 0x00, 0x00, 0x3c, 0x1c, 0x46, 0x40, 0x00,
        0x40, 0x06, 0xb1, 0xe6, 0xac, 0x10, 0x0a, 0x63,
        0xac, 0x10, 0x0a, 0x0c, 0x00, 0x50, 0xc0, 0x1e,
    };
    Log::info("Net", "packet:\n", Log::hexdump(pkt, sizeof(pkt)));

    for (int i = 0; i < 500; ++i)
        if (Log::sample<100>()) Log::info("Hot", "sampled ", Log::kv("i", i));

    for (int i = 0; i < 100; ++i)
        LOG_RATE_LIMITED_MS(info, 500, "Hot", "rate limited");

    for (int i = 0; i < 5; ++i)
        LOG_ONCE(info, "App", "printed once");
    for (int i = 0; i < 10; ++i)
        LOG_FIRST_N(info, 3, "App", "first 3 of 10");
    for (int i = 0; i < 100; ++i)
        LOG_EVERY_N(info, 25, "App", "every 25th");
    LOG_IF(warning, 2 + 2 == 4, "App", "math works");
    int* valid = &v[0];
    LOG_ASSERT(valid != nullptr, "App", "pointer check");

    Log::setContextFilter("App");
    Log::info("App", "visible");
    Log::info("DB", "filtered out");
    Log::clearContextFilter();

    Log::setContextFilter("-DB,-Cache");
    Log::info("App", "visible (block list)");
    Log::info("DB", "blocked");
    Log::clearContextFilter();

    Log::setThreadId(true);
    Log::info("App", "main thread");

    auto worker = [](int id) {
        for (int i = 0; i < 3; ++i)
            Log::info("Worker", "tick", Log::kv("id", id), Log::kv("i", i));
    };
    std::thread t1(worker, 1), t2(worker, 2);
    t1.join();
    t2.join();

    Log::info("App", "done");
}
