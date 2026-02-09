#include "log.h"
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <variant>
#include <vector>

enum class Status { OK, Error, Timeout };

int main() {
    Log::autoDetectColors();
    Log::setTimestamps(true);

    // --- levels ---
    Log::info("App", "starting up");
    Log::debug("App", "version=", 3, " build=", "release");
    Log::warning("App", "disk usage high");
    Log::error("App", "connection refused");

    // --- structured fields ---
    Log::info("HTTP", "request",
        Log::kv("method", "GET"),
        Log::kv("path", "/api/users"),
        Log::kv("status", 200),
        Log::kv("latency_ms", 3.14));

    // --- scoped fields ---
    {
        auto scope = Log::Scope(Log::kv("req_id", "abc-123"), Log::kv("user", 42));
        Log::info("DB", "query", Log::kv("rows", 100));
        Log::info("Cache", "miss", Log::kv("key", "user:42"));
        {
            auto inner = Log::Scope(Log::kv("tx", "write"));
            Log::info("DB", "insert", Log::kv("table", "events"));
        }
    }

    // --- timer ---
    {
        auto timer = Log::Timer("App", "computation");
        volatile int sum = 0;
        for (int i = 0; i < 1000000; ++i) sum += i;
    }

    // --- bool ---
    Log::info("Type", "bool: ", true, " ", false);

    // --- integers ---
    Log::info("Type", "int=", 42, " uint=", 123U, " i64=", static_cast<std::int64_t>(-99));

    // --- floats ---
    Log::info("Type", "pi=", 3.14159, " e=", 2.71828, " zero=", 0.0, " neg=", -1.5);
    Log::info("Type", "float=", 3.14f);

    // --- strings ---
    Log::info("Type", "cstr=", "hello");
    Log::info("Type", "sv=", std::string_view{"world"});
    Log::info("Type", "str=", std::string{"std::string"});
    Log::info("Type", "path=", std::filesystem::path{"/tmp/test.txt"});

    // --- pointers ---
    int x = 42;
    Log::info("Type", "ptr=", static_cast<void*>(&x), " null=", static_cast<void*>(nullptr));

    // --- byte ---
    Log::info("Type", "byte=", std::byte{0xAB});

    // --- nullptr / monostate ---
    Log::info("Type", "null=", nullptr, " mono=", std::monostate{});

    // --- optional ---
    std::optional<int> some = 42;
    std::optional<int> none;
    Log::info("Type", "some=", some, " none=", none);

    // --- variant ---
    std::variant<int, std::string> var = "hello";
    Log::info("Type", "variant=", var);

    // --- smart pointers ---
    auto sp = std::make_shared<int>(99);
    std::unique_ptr<int> np;
    Log::info("Type", "shared=", sp, " null=", np);

    // --- error_code / errc ---
    Log::info("Type", "ec=", std::make_error_code(std::errc::no_such_file_or_directory));
    Log::info("Type", "errc=", std::errc::invalid_argument);

    // --- chrono::duration ---
    using namespace std::chrono_literals;
    Log::info("Type", "dur=", 150ms, " fast=", 42us, " nano=", 999ns, " sec=", 3s);

    // --- chrono::time_point ---
    Log::info("Type", "now=", std::chrono::system_clock::now());

    // --- thread::id ---
    Log::info("Type", "tid=", std::this_thread::get_id());

    // --- source_location ---
    Log::info(Log::Loc{}, "Type", "with source location");

    // --- enum ---
    Log::info("Type", "status=", Status::OK);

    // --- pair / tuple ---
    Log::info("Type", "pair=", std::pair{42, "hello"});
    Log::info("Type", "tuple=", std::tuple{1, 3.14, "x"});

    // --- containers ---
    std::vector<int> v = {1, 2, 3, 4, 5};
    Log::info("Type", "vec=", v);
    std::map<std::string, int> m = {{"a", 1}, {"b", 2}};
    Log::info("Type", "map=", m);
    std::array<int, 3> arr = {10, 20, 30};
    Log::info("Type", "arr=", arr);

    // --- span<byte-like> ---
    const std::byte bytes[] = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
    Log::info("Type", "span=", std::span{bytes});

    // --- hexdump ---
    const unsigned char pkt[] = {
        0x45, 0x00, 0x00, 0x3c, 0x1c, 0x46, 0x40, 0x00,
        0x40, 0x06, 0xb1, 0xe6, 0xac, 0x10, 0x0a, 0x63,
        0xac, 0x10, 0x0a, 0x0c, 0x00, 0x50, 0xc0, 0x1e,
    };
    Log::info("Net", "packet:\n", Log::hexdump(pkt, sizeof(pkt)));
    Log::info("Net", "int dump:\n", Log::hexdump(x));

    // --- json output ---
    Log::setFormat(Log::JSON);
    Log::info("App", "json output",
        Log::kv("string", "hello \"world\""),
        Log::kv("num", 42),
        Log::kv("float", 3.14),
        Log::kv("flag", true));
    Log::setFormat(Log::Logfmt);

    // --- sampling + rate limiting ---
    for (int i = 0; i < 500; ++i)
        if (Log::sample<100>()) Log::info("Hot", "sampled ", Log::kv("i", i));
    for (int i = 0; i < 100; ++i)
        LOG_RATE_LIMITED_MS(info, 500, "Hot", "rate limited");

    // --- conditional macros ---
    for (int i = 0; i < 5; ++i)
        LOG_ONCE(info, "App", "printed once");
    for (int i = 0; i < 10; ++i)
        LOG_FIRST_N(info, 3, "App", "first 3 of 10");
    for (int i = 0; i < 100; ++i)
        LOG_EVERY_N(info, 25, "App", "every 25th");
    LOG_IF(warning, 2 + 2 == 4, "App", "math works");
    int* valid = &v[0];
    LOG_ASSERT(valid != nullptr, "App", "pointer check");
    LOG_DLOG("App", "debug-only log");

    // --- context filter ---
    Log::setContextFilter("App");
    Log::info("App", "visible");
    Log::info("DB", "filtered out");
    Log::clearContextFilter();

    Log::setContextFilter("-DB,-Cache");
    Log::info("App", "visible (block list)");
    Log::info("DB", "blocked");
    Log::clearContextFilter();

    // --- thread id + multithreading ---
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
