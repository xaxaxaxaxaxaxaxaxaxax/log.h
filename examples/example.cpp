#include "log.h"

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <numbers>
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
    Log::info("HTTP", "request", Log::kv("method", "GET"), Log::kv("path", "/api/users"), Log::kv("status", 200),
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
        auto         timer = Log::Timer("App", "computation");
        volatile int sum   = 0;
        for (int i = 0; i < 1000000; ++i)
            sum += i;
    }

    // --- bool ---
    Log::info("Type", "bool: ", true, " ", false);

    // --- integers ---
    Log::info("Type", "int=", 42, " uint=", 123U, " i64=", static_cast<std::int64_t>(-99));

    // --- floats ---
    Log::info("Type", "pi=", std::numbers::pi, " e=", std::numbers::e, " zero=", 0.0, " neg=", -1.5);
    Log::info("Type", "float=", 3.14f);

    // --- strings ---
    Log::info("Type", "cstr=", "hello");
    Log::info("Type", "sv=", std::string_view{"world"});
    Log::info("Type", "str=", std::string{"std::string"});
    Log::info("Type", "path=", std::filesystem::path{"/tmp/test.txt"});

    // --- pointers ---
    int x = 42; // non-const: address taken for pointer demo
    Log::info("Type", "ptr=", static_cast<void *>(&x), " null=", static_cast<void *>(nullptr));

    // --- byte ---
    Log::info("Type", "byte=", std::byte{0xAB});

    // --- nullptr / monostate ---
    Log::info("Type", "null=", nullptr, " mono=", std::monostate{});

    // --- optional ---
    constexpr std::optional      some = 42;
    constexpr std::optional<int> none;
    Log::info("Type", "some=", some, " none=", none);

    // --- variant ---
    constexpr std::variant<int, std::string_view> var = "hello";
    Log::info("Type", "variant=", var);

    // --- smart pointers ---
    const auto                     sp = std::make_shared<int>(99);
    constexpr std::unique_ptr<int> np;
    Log::info("Type", "shared=", sp, " null=", np);

    // --- error_code / errc ---
    Log::info("Type", "ec=", std::make_error_code(std::errc::no_such_file_or_directory));
    Log::info("Type", "errc=", std::errc::invalid_argument);

    // --- chrono::duration ---
    using std::chrono_literals::operator""ms;
    using std::chrono_literals::operator""us;
    using std::chrono_literals::operator""ns;
    using std::chrono_literals::operator""s;
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
    const std::vector v = {1, 2, 3, 4, 5};
    Log::info("Type", "vec=", v);
    const std::map<std::string, int> m = {{"a", 1}, {"b", 2}};
    Log::info("Type", "map=", m);
    constexpr std::array arr = {10, 20, 30};
    Log::info("Type", "arr=", arr);

    // --- span<byte-like> ---
    constexpr std::array bytes = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE}, std::byte{0xEF}};
    Log::info("Type", "span=", std::span{bytes});

    // --- hexdump ---
    constexpr std::array pkt = {
        std::byte{0x45}, std::byte{0x00}, std::byte{0x00}, std::byte{0x3c}, std::byte{0x1c}, std::byte{0x46},
        std::byte{0x40}, std::byte{0x00}, std::byte{0x40}, std::byte{0x06}, std::byte{0xb1}, std::byte{0xe6},
        std::byte{0xac}, std::byte{0x10}, std::byte{0x0a}, std::byte{0x63}, std::byte{0xac}, std::byte{0x10},
        std::byte{0x0a}, std::byte{0x0c}, std::byte{0x00}, std::byte{0x50}, std::byte{0xc0}, std::byte{0x1e},
    };
    Log::info("Net", "packet:\n", Log::hexdump(pkt.data(), pkt.size()));
    Log::info("Net", "int dump:\n", Log::hexdump(x));

    // --- json output ---
    Log::setFormat(Log::JSON);
    Log::info("App", "json output", Log::kv("string", "hello \"world\""), Log::kv("num", 42), Log::kv("float", 3.14),
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
    const int *const valid = v.data();
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
    const auto worker = [](const int id) {
        for (int i = 0; i < 3; ++i)
            Log::info("Worker", "tick", Log::kv("id", id), Log::kv("i", i));
    };
    std::thread t1(worker, 1);
    std::thread t2(worker, 2);
    t1.join();
    t2.join();

    Log::info("App", "done");
}
