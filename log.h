#pragma once
// log.h - zero-allocation structured logging (~100ns/op)
// Usage:    Log::info("Ctx", "msg"); Log::info("Ctx", Log::kv("k", v));
// JSON:     Log::setFormat(Log::JSON);
// Timer:    { auto t = Log::Timer("Ctx", "op"); }
// Scope:    { auto s = Log::Scope(Log::kv("id", 123)); Log::info("Ctx", "msg"); }
// ThreadId: Log::setThreadId(true);
// Filter:   Log::setContextFilter("Render,Audio");
// Sample:   if (Log::sample<100>()) Log::debug("Hot", "1-in-100");
// Hexdump:  Log::info("Net", "pkt: ", Log::hexdump(ptr, len));

#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <optional>
#include <ranges>
#include <ratio>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <variant>

// Platform detection
#if defined(__x86_64__) && defined(__linux__)
#include <sys/syscall.h>
#define LOG_FAST_WRITE 1
#else
#define LOG_FAST_WRITE 0
#endif

#if defined(__x86_64__) || defined(_M_X64)
#ifdef __AVX2__
#include <immintrin.h>
#define LOG_SIMD 4
#elifdef __SSE4_2__
#include <nmmintrin.h>
#define LOG_SIMD 3
#elifdef __SSSE3__
#include <tmmintrin.h>
#define LOG_SIMD 2
#elifdef __SSE2__
#include <emmintrin.h>
#define LOG_SIMD 1
#else
#define LOG_SIMD 0
#endif
#else
#define LOG_SIMD 0
#endif

#if __has_include(<stacktrace>) && __cpp_lib_stacktrace >= 202011L
#include <stacktrace>
#define LOG_HAS_STACKTRACE 1
#else
#define LOG_HAS_STACKTRACE 0
#endif

#if __has_include(<expected>) && __cpp_lib_expected >= 202202L
#include <expected>
#define LOG_HAS_EXPECTED 1
#else
#define LOG_HAS_EXPECTED 0
#endif

#if __has_include(<mdspan>) && __cpp_lib_mdspan >= 202207L
#include <mdspan>
#define LOG_HAS_MDSPAN 1
#else
#define LOG_HAS_MDSPAN 0
#endif

#if __has_include(<format>) && __cpp_lib_format >= 202110L
#include <format>
#define LOG_HAS_FORMAT 1
#else
#define LOG_HAS_FORMAT 0
#endif

#ifndef LOG_LEVEL
#define LOG_LEVEL 0
#endif
#ifndef LOG_MAX_SCOPED
#define LOG_MAX_SCOPED 8
#endif
#ifndef LOG_MAX_FILTERS
#define LOG_MAX_FILTERS 16
#endif

namespace logging::inline v1 {

class Log;

namespace detail {

template<typename T> using Clean = std::remove_cvref_t<T>;
template<typename T, typename... Us>
concept OneOf = (std::same_as<Clean<T>, Us> || ...);

template<typename T>
concept NullLike = OneOf<T, std::nullptr_t, std::monostate>;
template<typename T>
concept StringLike = OneOf<T, std::string, std::string_view, std::filesystem::path> ||
                     std::same_as<Clean<T>, const char*> || std::same_as<Clean<T>, char*>;
template<typename T>
concept Rangeable = std::ranges::range<T> && !StringLike<T>;
template<typename T>
concept CharLike = std::integral<T> && sizeof(T) == 1 && !std::same_as<T, bool>;
template<typename T>
concept LoggableInt = std::integral<T> && sizeof(T) > 1 && !std::same_as<T, bool>;
template<typename T>
concept ByteLike = sizeof(T) == 1 && std::is_trivially_copyable_v<T> &&
                   OneOf<std::remove_cv_t<T>, std::byte, unsigned char, char>;
template<typename T>
concept Pair = requires(T t) {
    t.first;
    t.second;
};
template<typename T>
concept Variant = requires(T t) {
    t.index();
    t.valueless_by_exception();
};
template<typename T>
concept Aggregate = !Variant<T> && (Pair<T> || (requires {
                                        std::tuple_size_v<Clean<T>>;
                                        std::get<0>(std::declval<T>());
                                    } && !Rangeable<T>));
template<typename T>
concept SmartPtr = !std::is_pointer_v<T> && requires(T t) {
    *t;
    static_cast<bool>(t);
    t.get();
};
#if LOG_HAS_EXPECTED
template<typename T>
concept Expected = requires(T t) {
    t.has_value();
    *t;
    t.error();
};
#endif
#if LOG_HAS_MDSPAN
template<typename T>
concept MdSpanLike = requires(T t) {
    Clean<T>::rank();
    t.extent(0);
};
#endif
#if LOG_HAS_FORMAT
template<typename T>
concept Formattable = requires { std::formatter<Clean<T>>{}; };
#endif

inline constexpr char kHex[] = "0123456789abcdef";
inline constexpr std::array<std::string_view, 2> kBool = {"false", "true"};

template<std::size_t W, std::size_t N>
inline constexpr auto kDigits = [] static consteval {
    std::array<std::array<char, W>, N> t{};
    for (std::size_t v = 0; v < N; ++v) {
        for (auto val = v, i = W; i-- > 0; val /= 10) {
            t[v][i] = static_cast<char>('0' + val % 10);
        }
    }
    return t;
}();

inline constexpr auto kHexPair = [] static consteval {
    std::array<std::array<char, 2>, 256> t{};
    for (unsigned i = 0; i < 256; ++i) {
        t[i] = {kHex[i >> 4], kHex[i & 0xF]};
    }
    return t;
}();

inline constexpr auto kJsonEsc = [] static consteval {
    std::array<unsigned char, 256> t{};
    for (unsigned i = 0; i < 32; ++i) {
        t[i] = 0xFF;
    }
    t['\b'] = 'b';
    t['\t'] = 't';
    t['\n'] = 'n';
    t['\f'] = 'f';
    t['\r'] = 'r';
    t['"'] = '"';
    t['\\'] = '\\';
    return t;
}();

inline constexpr auto kJsonUni = [] static consteval {
    std::array<std::array<char, 6>, 32> t{};
    for (unsigned i = 0; i < 32; ++i) {
        t[i] = {'\\', 'u', '0', '0', kHex[i >> 4], kHex[i & 0xF]};
    }
    return t;
}();

inline constexpr auto& kDigit2 = kDigits<2, 100>;
inline constexpr auto& kDigit3 = kDigits<3, 1000>;
inline constexpr auto& kDigit4 = kDigits<4, 10000>;

template<std::size_t N> [[gnu::always_inline]] inline void copyN(char* d, const auto& s) noexcept {
    __builtin_memcpy(d, s.data(), N);
}

namespace simd {

[[gnu::always_inline]] constexpr auto div1000(const std::uint32_t v) noexcept {
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(v) * 274877907ULL) >> 38);
}
[[gnu::always_inline]] constexpr auto div1M(const std::uint64_t v) noexcept {
    return static_cast<std::uint32_t>(v / 1000000);
}

template<unsigned N>
    requires(N >= 2 && N <= 8)
[[gnu::always_inline]] constexpr std::conditional_t<N <= 4, std::uint32_t, std::uint64_t> packDig(
    const std::uint32_t v) noexcept {
    if constexpr (N == 2) {
        const auto d = (v * 103U) >> 10;
        return (d + '0') | (v - d * 10 + '0') << 8;
    } else if constexpr (N == 3) {
        return packDig<2>((v * 205U) >> 11) | (v % 10 + '0') << 16;
    } else if constexpr (N == 4) {
        const auto q = (v * 5243U) >> 19;
        return packDig<2>(q) | packDig<2>(v - q * 100) << 16;
    } else if constexpr (N == 6) {
        const auto h = div1000(v);
        return static_cast<std::uint64_t>(packDig<3>(h)) |
               static_cast<std::uint64_t>(packDig<3>(v - h * 1000)) << 24;
    } else {
        return static_cast<std::uint64_t>(packDig<4>(v / 10000)) |
               static_cast<std::uint64_t>(packDig<4>(v % 10000)) << 32;
    }
}

#if LOG_SIMD >= 1 // SSE2
inline const __m128i kSp = _mm_set1_epi8(0x20);
inline const __m128i kQt = _mm_set1_epi8('"');
inline const __m128i kBs = _mm_set1_epi8('\\');

[[gnu::always_inline]] inline int escMask16(const void* p) noexcept {
    const auto c = _mm_loadu_si128(static_cast<const __m128i*>(p));
    return _mm_movemask_epi8(_mm_or_si128(
        _mm_or_si128(_mm_cmplt_epi8(c, kSp), _mm_cmpeq_epi8(c, kQt)), _mm_cmpeq_epi8(c, kBs)));
}
#endif

#if LOG_SIMD >= 2 // SSSE3
inline const __m128i kHexLut =
    _mm_setr_epi8('0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f');
inline const __m128i kMaskLo = _mm_set1_epi8(0x0F);
inline const __m128i kDel = _mm_set1_epi8(0x7F);
inline const __m128i kDot = _mm_set1_epi8('.');

[[gnu::always_inline]] inline __m128i hexEnc(__m128i b) noexcept {
    return _mm_shuffle_epi8(
        kHexLut,
        _mm_unpacklo_epi8(_mm_and_si128(_mm_srli_epi16(b, 4), kMaskLo), _mm_and_si128(b, kMaskLo)));
}

[[gnu::always_inline]] inline __m128i toPrint(__m128i b) noexcept {
    return _mm_blendv_epi8(b, kDot, _mm_or_si128(_mm_cmplt_epi8(b, kSp), _mm_cmpeq_epi8(b, kDel)));
}

[[gnu::always_inline]] inline void hex64(char* o, std::uint64_t v) noexcept {
    _mm_storeu_si128(reinterpret_cast<__m128i*>(o),
                     hexEnc(_mm_set1_epi64x(static_cast<long long>(std::byteswap(v)))));
}

[[gnu::always_inline]] inline void hex16(char* o, std::uint16_t v) noexcept {
    _mm_storeu_si32(o, hexEnc(_mm_cvtsi32_si128(std::byteswap(v))));
}

template<bool Sp> [[gnu::always_inline]] inline char* scatter8(char* o, __m128i h) noexcept {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (([&] {
             auto p = static_cast<std::uint16_t>(_mm_extract_epi16(h, I));
             __builtin_memcpy(o + (I * 3), &p, 2);
             if constexpr (Sp && I < 7) {
                 o[(I * 3) + 2] = ' ';
             }
         }()),
         ...);
    }(std::make_index_sequence<8>{});
    return o + (Sp ? 23 : 24);
}
#endif

#if LOG_SIMD >= 3 // SSE4.2
[[gnu::always_inline]] inline std::size_t crc32(const char* s, std::size_t n) noexcept {
    std::uint64_t h = 0;
    for (; n >= 8; s += 8, n -= 8) {
        std::uint64_t v{};
        __builtin_memcpy(&v, s, 8);
        h = _mm_crc32_u64(h, v);
    }
    while (n--) {
        h = _mm_crc32_u8(static_cast<std::uint32_t>(h), static_cast<unsigned char>(*s++));
    }
    return h;
}
#endif

#if LOG_SIMD >= 4 // AVX2
inline const __m256i kSp32 = _mm256_set1_epi8(0x20);
inline const __m256i kQt32 = _mm256_set1_epi8('"');
inline const __m256i kBs32 = _mm256_set1_epi8('\\');

[[gnu::always_inline]] inline int escMask32(const void* p) noexcept {
    auto c = _mm256_loadu_si256(static_cast<const __m256i*>(p));
    return _mm256_movemask_epi8(
        _mm256_or_si256(_mm256_or_si256(_mm256_cmpgt_epi8(kSp32, c), _mm256_cmpeq_epi8(c, kQt32)),
                        _mm256_cmpeq_epi8(c, kBs32)));
}

[[gnu::always_inline]] inline int search4(const std::size_t* a, std::size_t h) noexcept {
    return _mm256_movemask_pd(_mm256_castsi256_pd(
        _mm256_cmpeq_epi64(_mm256_set1_epi64x(static_cast<long long>(h)),
                           _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a)))));
}
#endif

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overread"
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
[[gnu::always_inline]] inline void copySmall(void* d, const void* s, const std::size_t n) noexcept {
    if (n == 0U) [[unlikely]] {
        return;
    }
    auto* dst = static_cast<char*>(d);
    const auto* src = static_cast<const char*>(s);
    auto copyHT = [=]<typename T>() {
        T h;
        T t;
        __builtin_memcpy(&h, src, sizeof(T));
        __builtin_memcpy(&t, src + n - sizeof(T), sizeof(T));
        __builtin_memcpy(dst, &h, sizeof(T));
        __builtin_memcpy(dst + n - sizeof(T), &t, sizeof(T));
    };
    if (n == 1) {
        *dst = *src;
        return;
    }
    if (n <= 4) {
        copyHT.operator()<std::uint16_t>();
        return;
    }
    if (n <= 8) {
        copyHT.operator()<std::uint32_t>();
        return;
    }
    if (n <= 16) {
        copyHT.operator()<std::uint64_t>();
        return;
    }
#if LOG_SIMD >= 1
    if (n <= 32) {
        const auto h = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src));
        const auto t = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + n - 16));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), h);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + n - 16), t);
        return;
    }
#endif
#if LOG_SIMD >= 4
    if (n <= 64) {
        auto h = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src)),
             t = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + n - 32));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst), h);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + n - 32), t);
        return;
    }
#endif
    __builtin_memcpy(dst, src, n);
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

} // namespace simd

[[nodiscard]] constexpr char toAsciiDot(const unsigned char c) noexcept {
    return c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.';
}

inline constexpr std::string_view kReset = "\033[0m", kDim = "\033[2m";
inline constexpr std::array<std::string_view, 4> kLvl = {
    "[DEBUG] [", "[INFO ] [", "[WARN ] [", "[ERROR] ["};
inline constexpr std::array<std::string_view, 4> kLvlC = {"\033[36m[DEBUG]\033[0m [",
                                                          "\033[32m[INFO ]\033[0m [",
                                                          "\033[33m[WARN ]\033[0m [",
                                                          "\033[31m[ERROR]\033[0m ["};
inline constexpr std::array<std::string_view, 4> kJsonHdr = {R"({"level":"debug","ctx":")",
                                                             R"({"level":"info","ctx":")",
                                                             R"({"level":"warning","ctx":")",
                                                             R"({"level":"error","ctx":")"};

struct alignas(64) TsCache {
    std::time_t sec{};
    std::array<char, 12> hms{};
    std::array<char, 24> iso{};

    [[gnu::noinline, gnu::cold]] void update(const std::time_t s) noexcept {
        sec = s;
        std::tm tm{};
        localtime_r(&s, &tm);
        auto fmt2 = [](char* p, const unsigned v, const char c) static {
            copyN<2>(p, kDigit2[v]);
            p[2] = c;
        };
        fmt2(hms.data(), static_cast<unsigned>(tm.tm_hour), ':');
        fmt2(hms.data() + 3, static_cast<unsigned>(tm.tm_min), ':');
        fmt2(hms.data() + 6, static_cast<unsigned>(tm.tm_sec), '.');
        copyN<4>(iso.data(),
                 kDigit4[static_cast<std::size_t>(
                     1900 + tm.tm_year)]); // NOLINT(bugprone-misplaced-widening-cast)
        iso[4] = '-';
        fmt2(iso.data() + 5, static_cast<unsigned>(1 + tm.tm_mon), '-');
        fmt2(iso.data() + 8, static_cast<unsigned>(tm.tm_mday), 'T');
        __builtin_memcpy(iso.data() + 11, hms.data(), 9);
    }

    template<bool Iso>
    [[nodiscard, gnu::always_inline]]
    std::string_view get(const std::time_t s, const unsigned ms) noexcept {
        if (s != sec) [[unlikely]] {
            update(s);
        }
        if constexpr (Iso) {
            copyN<3>(iso.data() + 20, kDigit3[ms % 1000]);
            iso[23] = 'Z';
            return {iso.data(), 24};
        } else {
            copyN<3>(hms.data() + 9, kDigit3[ms % 1000]);
            return {hms.data(), 12};
        }
    }
};
[[nodiscard]] inline TsCache& tsCache() noexcept {
    thread_local TsCache c;
    return c;
}

struct ScopedFields {
    using WriteFn = void (*)(void*, const void*, bool);
    struct Entry {
        std::string_view key;
        const void* val;
        WriteFn write;
    };
    std::array<Entry, LOG_MAX_SCOPED> f{};
    std::size_t n{};
};
[[nodiscard]] inline ScopedFields& scopedFields() noexcept {
    thread_local ScopedFields s;
    return s;
}

struct TidCache {
    std::array<char, 32> lf{}, js{};
    std::size_t lfLen{23}, jsLen{27};

    TidCache() noexcept {
        const auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
        auto hex = [tid](char* p) {
#if LOG_SIMD >= 2
            simd::hex64(p, tid);
#else
            for (unsigned i = 0; i < 16; ++i) {
                p[i] = kHex[tid >> (15 - i) * 4 & 0xF];
            }
#endif
        };
        __builtin_memcpy(lf.data(), "tid=0x", 6);
        hex(lf.data() + 6);
        lf[22] = ' ';
        __builtin_memcpy(js.data(), R"(,"tid":"0x)", 10);
        hex(js.data() + 10);
        js[26] = '"';
    }
    [[nodiscard]] std::string_view get(const bool json) const noexcept {
        return json ? std::string_view{js.data(), jsLen} : std::string_view{lf.data(), lfLen};
    }
};
[[nodiscard]] inline const TidCache& tidCache() noexcept {
    thread_local const TidCache c;
    return c;
}

[[nodiscard]] constexpr std::size_t fnv1a(const std::string_view s) noexcept {
    std::size_t h = 14695981039346656037ULL;
    for (const char ch : s) {
        h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ULL;
    }
    return h;
}
[[nodiscard]] constexpr std::size_t fastHash(const std::string_view s) noexcept {
    if consteval { // NOLINT(readability-braces-around-statements)
        return fnv1a(s);
    }
#if LOG_SIMD >= 3
    return simd::crc32(s.data(), s.size());
#else
    return fnv1a(s);
#endif
}

struct CtxFilter {
    alignas(32) std::array<std::size_t, LOG_MAX_FILTERS> allow{}, block{};
    std::size_t aCnt{}, bCnt{};

    [[nodiscard]] static bool search(const std::size_t* a, const std::size_t n, const std::size_t h) noexcept {
        std::size_t i = 0;
#if LOG_SIMD >= 4
        for (; i + 4 <= n; i += 4) {
            if (simd::search4(a + i, h)) {
                return true;
            }
        }
#endif
        for (; i < n; ++i) {
            if (a[i] == h) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] bool empty() const noexcept {
        return aCnt == 0U && bCnt == 0U;
    }
    [[nodiscard]] bool check(const std::size_t h) const noexcept {
        return aCnt != 0U ? search(allow.data(), aCnt, h) : !search(block.data(), bCnt, h);
    }
    void clear() noexcept {
        aCnt = bCnt = 0;
    }

    void parse(std::string_view spec) noexcept {
        clear();
        for (auto tok :
             spec | std::views::split(',') | std::views::transform([](auto&& r) static {
                 return std::string_view{r.begin(), r.end()};
             }) | std::views::transform([](const std::string_view s) static {
                 const auto b = s.find_first_not_of(' ');
                 if (b == std::string_view::npos) return std::string_view{};
                 return s.substr(b, s.find_last_not_of(' ') - b + 1);
             }) | std::views::filter([](const std::string_view s) static { return !s.empty(); })) {
            const bool blk = tok.front() == '-';
            auto sv = blk ? tok.substr(1) : tok;
            if (sv.empty()) {
                continue;
            }
            auto& arr = blk ? block : allow;
            if (auto& cnt = blk ? bCnt : aCnt; cnt < LOG_MAX_FILTERS) {
                arr[cnt++] = fastHash(sv);
            }
        }
    }
};
inline CtxFilter& ctxFilter() noexcept {
    static CtxFilter f;
    return f;
}

} // namespace detail

class Log {
public:
    enum class Level : std::int8_t { Debug = 0, Info = 1, Warning = 2, Error = 3, None = 4 };
    using enum Level;
    enum class Format : std::int8_t { Logfmt = 0, JSON = 1 };
    using enum Format;

    template<typename T> struct Field {
        std::string_view key;
        T value;
    };
    template<typename T> Field(std::string_view, T) -> Field<T>;
    template<typename T>
    [[nodiscard]] static constexpr auto kv(std::string_view k, T&& v) noexcept {
        return Field{k, std::forward<T>(v)};
    }
    template<typename T> using F = Field<T>;

    struct HexDump {
        const void* data;
        std::size_t size, max;
        bool ascii = true;
        constexpr HexDump(const void* d,
                          const std::size_t s,
                          const std::size_t m = 64) noexcept
            : data{d}, size{s}, max{m} {}
    };
    [[nodiscard]] static constexpr auto hexdump(const void* d,
                                                const std::size_t s,
                                                const std::size_t m = 64) noexcept {
        return HexDump{d, s, m};
    }
    template<typename T>
        requires std::is_trivially_copyable_v<T>
    [[nodiscard]] static constexpr auto hexdump(const T& o) noexcept {
        return HexDump{&o, sizeof(T), sizeof(T)};
    }

    template<typename... Fs> class [[nodiscard]] Scope {
        std::tuple<std::decay_t<Fs>...> fields_;
        std::size_t prev_;
        template<typename T> static void add(detail::ScopedFields& s, const Field<T>& f) noexcept {
            if (s.n >= LOG_MAX_SCOPED) [[unlikely]] {
                return;
            }
            s.f[s.n++] = {f.key, &f.value, [](void* w, const void* v, bool j) static {
                              auto& wr = *static_cast<Writer*>(w);
                              j ? wr.putJsonVal(*static_cast<const T*>(v))
                                : wr.put(*static_cast<const T*>(v));
                          }};
        }

    public:
        explicit Scope(Fs&&... fs) noexcept
            : fields_{std::forward<Fs>(fs)...}, prev_{detail::scopedFields().n} {
            auto& s = detail::scopedFields();
            [&]<std::size_t... I>(std::index_sequence<I...>) {
                (add(s, std::get<I>(fields_)), ...);
            }(std::index_sequence_for<Fs...>{});
        }
        ~Scope() noexcept {
            detail::scopedFields().n = prev_;
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        Scope(Scope&&) = delete;
        Scope& operator=(Scope&&) = delete;
    };
    template<typename... Fs> Scope(Fs&&...) -> Scope<Fs...>;

    template<typename T>
        requires std::same_as<T, Level> || std::same_as<T, int>
    static void setMinLevel(T l) noexcept {
        if constexpr (std::same_as<T, Level>) {
            sMin = std::to_underlying(l);
        } else {
            sMin = l;
        }
    }
    [[nodiscard]] static int minLevel() noexcept {
        return sMin;
    }
    static void setTimestamps(const bool e) noexcept {
        sTs = e;
    }
    [[nodiscard]] static bool timestamps() noexcept {
        return sTs;
    }
    static void setColors(const bool e) noexcept {
        sCol = e;
    }
    [[nodiscard]] static bool colors() noexcept {
        return sCol;
    }
    static void autoDetectColors() noexcept {
        sCol = isatty(STDERR_FILENO) != 0;
    }
    static void setFormat(const Format f) noexcept {
        sFmt = f;
    }
    [[nodiscard]] static Format format() noexcept {
        return sFmt;
    }
    static void setThreadId(const bool e) noexcept {
        sTid = e;
    }
    [[nodiscard]] static bool threadId() noexcept {
        return sTid;
    }
    static void setContextFilter(const std::string_view s) noexcept {
        detail::ctxFilter().parse(s);
    }
    static void clearContextFilter() noexcept {
        detail::ctxFilter().clear();
    }
    [[nodiscard]] static bool contextAllowed(const std::string_view c) noexcept {
        const auto& f = detail::ctxFilter();
        return f.empty() || f.check(detail::fastHash(c));
    }

    template<int Id> [[nodiscard]] static bool rateLimit(const std::chrono::milliseconds iv) noexcept {
        using C = std::chrono::steady_clock;
        thread_local C::time_point last{};
        if (const auto now = C::now(); now - last >= iv) [[unlikely]] {
            last = now;
            return true;
        }
        return false;
    }

    template<unsigned N>
        requires(N > 0)
    [[nodiscard]] static bool sample() noexcept {
        if constexpr (N == 1) {
            return true;
        } else if constexpr (std::has_single_bit(N)) {
            thread_local unsigned c = 0;
            return (++c & N - 1) == 0;
        } else {
            if (thread_local unsigned c = 0; ++c >= N) [[unlikely]] {
                c = 0;
                return true;
            }
            return false;
        }
    }

    class [[nodiscard("Timer must be stored")]] Timer {
        std::chrono::steady_clock::time_point start_{std::chrono::steady_clock::now()};
        std::string_view ctx_, name_;

    public:
        Timer(const std::string_view c, const std::string_view n) noexcept : ctx_{c}, name_{n} {}
        ~Timer() noexcept {
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - start_)
                                .count();
            us < 1000 ? debug(ctx_, name_, Field{"elapsed_us", us})
                      : debug(ctx_, name_, Field{"elapsed_ms", static_cast<double>(us) / 1000.0});
        }
        Timer(const Timer&) = delete;
        Timer& operator=(const Timer&) = delete;
        Timer(Timer&&) = delete;
        Timer& operator=(Timer&&) = delete;
    };

    struct Loc {
        std::source_location loc;
        explicit constexpr Loc(const std::source_location l = std::source_location::current()) noexcept
            : loc{l} {}
    };

    template<int L, typename... Args>
    static void log([[maybe_unused]] std::string_view ctx,
                    [[maybe_unused]] Args&&... args) noexcept {
        if constexpr (LOG_LEVEL <= L) {
            if (sMin <= L && contextAllowed(ctx)) [[unlikely]] {
                emit<L>(ctx, std::forward<Args>(args)...);
            }
        }
    }
    template<int L, typename... Args>
    static void log([[maybe_unused]] Loc l,
                    [[maybe_unused]] std::string_view ctx,
                    [[maybe_unused]] Args&&... args) noexcept {
        if constexpr (LOG_LEVEL <= L) {
            if (sMin <= L && contextAllowed(ctx)) [[unlikely]] {
                emitLoc<L>(ctx, l.loc, std::forward<Args>(args)...);
            }
        }
    }
    static void debug(auto&&... a) noexcept {
        log<0>(std::forward<decltype(a)>(a)...);
    }
    static void info(auto&&... a) noexcept {
        log<1>(std::forward<decltype(a)>(a)...);
    }
    static void warning(auto&&... a) noexcept {
        log<2>(std::forward<decltype(a)>(a)...);
    }
    static void error(auto&&... a) noexcept {
        log<3>(std::forward<decltype(a)>(a)...);
    }

#if LOG_HAS_STACKTRACE
    struct Stack {
        std::stacktrace trace;
        Stack(std::stacktrace t = std::stacktrace::current()) noexcept : trace{std::move(t)} {}
    };
    template<typename... Args>
    static void error([[maybe_unused]] Stack s,
                      [[maybe_unused]] std::string_view ctx,
                      [[maybe_unused]] Args&&... args) noexcept {
        if constexpr (LOG_LEVEL <= 3) {
            if (sMin <= 3) {
                emitStack<3>(ctx, s.trace, std::forward<Args>(args)...);
            }
        }
    }
#endif

private:
    static inline constinit int sMin = 0;
    static inline constinit bool sTs = false;
    static inline constinit bool sCol = false;
    static inline constinit bool sTid = false;
    static inline constinit auto sFmt = Logfmt;
    static constexpr std::size_t kBuf = 4096;

    struct Writer {
        char* const begin;
        char* pos;
        char* const end;

        constexpr explicit Writer(std::span<char> b) noexcept
            : begin{b.data()}, pos{b.data()}, end{b.data() + b.size() - 1} {}

        [[nodiscard]] constexpr std::size_t rem() const noexcept {
            [[assume(pos <= end)]];
            return static_cast<std::size_t>(end - pos);
        }
        [[nodiscard]] constexpr std::size_t len() const noexcept {
            [[assume(pos >= begin)]];
            return static_cast<std::size_t>(pos - begin);
        }

        constexpr void put(const char c) noexcept {
            [[assume(pos <= end)]];
            *pos = c;
            pos += static_cast<int>(pos < end);
        }
        constexpr void putNull() noexcept {
            putFixed("null");
        }

        template<typename A> constexpr void putFixed(const A& a) noexcept {
            constexpr auto N = std::size(A{});
            constexpr auto n = N - (std::same_as<std::remove_extent_t<A>, char> ? 1 : 0);
            if (rem() >= n) [[likely]] {
                __builtin_memcpy(pos, std::data(a), n);
                pos += n;
            } else {
                putSV({static_cast<const char*>(std::data(a)), n});
            }
        }

        void putSV(const std::string_view s) noexcept {
            if (s.size() <= rem()) [[likely]] {
                detail::simd::copySmall(pos, s.data(), s.size());
                pos += s.size();
            } else {
                const auto n = rem();
                __builtin_memcpy(pos, s.data(), n);
                pos += n;
            }
        }

        template<std::size_t N, typename Fn> void putNum(Fn&& f) noexcept {
            if (rem() >= N) [[likely]] {
                auto [p, ec] = f(pos, end);
                [[assume(ec == std::errc{})]];
                pos = p;
            } else {
                std::array<char, N> t{};
                auto [p, ec] = f(t.data(), t.data() + N);
                [[assume(ec == std::errc{})]];
                putSV({t.data(), static_cast<std::size_t>(p - t.data())});
            }
        }

        template<typename T>
        [[nodiscard]] static constexpr std::string_view toSV(const T& v) noexcept {
            if constexpr (std::same_as<detail::Clean<T>, const char*> ||
                          std::same_as<detail::Clean<T>, char*>) {
                return v ? v : "";
            } else if constexpr (std::same_as<detail::Clean<T>, std::filesystem::path>) {
                return v.native();
            } else {
                return v;
            }
        }

        template<typename T>
            requires detail::StringLike<T> || std::same_as<detail::Clean<T>, bool> ||
                     detail::NullLike<T>
        constexpr void put(const T& v) noexcept {
            if constexpr (detail::NullLike<T>) {
                putNull();
            } else if constexpr (std::same_as<detail::Clean<T>, bool>) {
                putSV(detail::kBool[v]);
            } else {
                putSV(toSV(v));
            }
        }
        template<std::size_t N> void put(const char (&s)[N]) noexcept {
            putSV({s, N - 1});
        }

        void put(std::nullptr_t) noexcept {
            putNull();
        }
        void put(const void* p) noexcept {
            if (p == nullptr) [[unlikely]] {
                putNull();
                return;
            }
            putHex(std::bit_cast<std::uintptr_t>(p));
        }

        void putHexByte(const unsigned char b) noexcept {
            detail::copyN<2>(pos, detail::kHexPair[b]);
            pos += 2;
        }

        template<std::unsigned_integral T> void putHexRaw(T v) noexcept {
            if (!v) [[unlikely]] {
                *pos++ = '0';
                return;
            }
#if LOG_SIMD >= 2
            if constexpr (sizeof(T) == 8) {
                auto lz = static_cast<unsigned>(std::countl_zero(v));
                auto skip = lz / 8;
                if (skip < 4 && rem() >= 16) {
                    alignas(16) char buf[16];
                    detail::simd::hex64(buf, v);
                    auto sk = (skip * 2) + ((v >> (((7 - skip) * 8) + 4)) == 0);
                    auto n = 16 - sk;
                    __builtin_memcpy(pos, buf + sk, n);
                    pos += n;
                    return;
                }
            }
#endif
            const auto lz = static_cast<std::size_t>(std::countl_zero(v));
            const auto skip = lz / 8;
            const auto start = sizeof(T) - 1 - skip;
            if (const auto first = static_cast<unsigned char>(v >> start * 8 & 0xFF);
                first >> 4 == 0) {
                *pos++ = detail::kHexPair[first][1];
            } else {
                putHexByte(first);
            }
            for (std::size_t i = start; i-- > 0;) {
                putHexByte(static_cast<unsigned char>(v >> (i * 8) & 0xFF));
            }
        }

        template<std::unsigned_integral T> void putHex(T v) noexcept {
            if (constexpr auto D = sizeof(T) * 2; rem() < D + 2) [[unlikely]] {
                putNum<D + 2>([v](char* b, char* e) {
                    *b++ = '0';
                    *b++ = 'x';
                    return std::to_chars(b, e, v, 16);
                });
                return;
            }
            *pos++ = '0';
            *pos++ = 'x';
            putHexRaw(v);
        }

        void put(const std::thread::id id) noexcept {
            putSV("tid:");
            putHex(std::hash<std::thread::id>{}(id));
        }
        void put(const std::byte b) noexcept {
            putHexByte(std::to_integer<unsigned char>(b));
        }

        [[gnu::always_inline]] void put8DigFast(const std::uint32_t v) noexcept {
#if LOG_SIMD >= 2
            std::uint64_t d = detail::simd::packDig<8>(v);
            __builtin_memcpy(pos, &d, 8);
            pos += 8;
#else
            putFixed(detail::kDigit4[v / 10000]);
            putFixed(detail::kDigit4[v % 10000]);
#endif
        }

        template<bool Pad = false> void putSmallU(const std::uint32_t v) noexcept {
            if constexpr (Pad) {
                putFixed(detail::kDigit2[v / 100]);
                putFixed(detail::kDigit2[v % 100]);
            } else if (v < 10) [[likely]] {
                put(static_cast<char>('0' + v));
            } else if (v < 100) {
                putFixed(detail::kDigit2[v]);
            } else if (v < 1000) {
                putFixed(detail::kDigit3[v]);
            } else {
                putFixed(detail::kDigit4[v]);
            }
        }

        template<std::unsigned_integral T> void putU(T v) noexcept {
            if (v < 10000) [[likely]] {
                putSmallU(static_cast<std::uint32_t>(v));
                return;
            }
            putULarge(v);
        }

        void put8DigPadded(const std::uint32_t lo) noexcept {
            if (rem() >= 8) [[likely]] {
                put8DigFast(lo);
                return;
            }
            putFixed(detail::kDigit4[lo / 10000]);
            putFixed(detail::kDigit4[lo % 10000]);
        }

        template<std::unsigned_integral T> [[gnu::noinline]] void putULarge(T v) noexcept {
            constexpr std::uint64_t k1e8 = 100000000ULL;
            if (v < k1e8) {
                if (v >= 10000000 && rem() >= 8) [[likely]] {
                    put8DigFast(static_cast<std::uint32_t>(v));
                    return;
                }
                putSmallU(static_cast<std::uint32_t>(v / 10000));
                putSmallU<true>(static_cast<std::uint32_t>(v % 10000));
                return;
            }
            if constexpr (sizeof(T) <= 4) {
                putSmallU(static_cast<std::uint32_t>(v / k1e8));
                put8DigPadded(static_cast<std::uint32_t>(v % k1e8));
            } else {
                constexpr std::uint64_t k1e16 = k1e8 * k1e8;
                if (v < k1e16) {
                    putULarge(static_cast<std::uint32_t>(v / k1e8));
                    put8DigPadded(static_cast<std::uint32_t>(v % k1e8));
                } else {
                    putNum<24>([v](char* b, char* e) { return std::to_chars(b, e, v); });
                }
            }
        }

        template<detail::LoggableInt T> void put(T v) noexcept {
            if constexpr (std::unsigned_integral<T>) {
                putU(v);
            } else {
                if (v < 0) {
                    put('-');
                    putU(std::make_unsigned_t<T>(0) - std::make_unsigned_t<T>(v));
                } else {
                    putU(std::make_unsigned_t<T>(v));
                }
            }
        }

        template<detail::CharLike T> void put(T v) noexcept {
            if constexpr (std::is_signed_v<T>) {
                put(static_cast<int>(v));
            } else {
                put(static_cast<unsigned>(v));
            }
        }

        template<std::floating_point T> void put(T v) noexcept {
            using B = std::conditional_t<sizeof(T) <= 4, std::uint32_t, std::uint64_t>;
            constexpr B kExp = [] {
                if constexpr (sizeof(T) <= 4) {
                    return 0x7F800000U;
                } else {
                    return 0x7FF0000000000000ULL;
                }
            }();
            if ((std::bit_cast<B>(v) & kExp) != kExp && v > T(-1e9) && v < T(1e9)) [[likely]] {
                if (auto iv = static_cast<long long>(v); v == T(iv)) [[likely]] {
                    put(iv);
                    putSV(".0");
                    return;
                }
                if (v > T(-1e6) && v < T(1e6) && tryFixed(v)) {
                    return;
                }
            }
            putNum<64>([v](char* b, char* e) { return std::to_chars(b, e, v); });
        }

        template<std::floating_point T> [[nodiscard]] bool tryFixed(T v) noexcept {
            constexpr T kScale = T(1000000);
            T scaled = v * kScale;
            auto isc = static_cast<long long>(scaled + (scaled >= 0 ? T(0.5) : T(-0.5)));
            if (std::abs(T(isc) / kScale - v) >= T(1e-9) + std::abs(v) * T(1e-6)) [[unlikely]] {
                return false;
            }
            const bool neg = isc < 0;
            const auto abs = static_cast<std::uint64_t>(neg ? -isc : isc);
            const std::uint64_t whole = detail::simd::div1M(abs);
            const auto frac = static_cast<std::uint32_t>(abs - whole * 1000000);
            if (neg) {
                put('-');
            }
            putU(whole);
            put('.');
            if (!frac) {
                put('0');
                return true;
            }
            putFrac(frac);
            return true;
        }

        void putFrac(const std::uint32_t f) noexcept {
            char buf[6];
#if LOG_SIMD >= 2
            std::uint64_t d = detail::simd::packDig<6>(f);
            __builtin_memcpy(buf, &d, 6);
#else
            const auto hi = detail::simd::div1000(f);
            detail::copyN<3>(buf, detail::kDigit3[hi]);
            detail::copyN<3>(buf + 3, detail::kDigit3[f - hi * 1000]);
#endif
            std::size_t n = 6;
            while (n > 1 && buf[n - 1] == '0') {
                --n;
            }
            putSV({buf, n});
        }

        template<typename T>
            requires std::is_enum_v<T>
        void put(T v) noexcept {
            put(std::to_underlying(v));
        }

        template<typename T> void put(const std::optional<T>& o) noexcept {
            o ? put(*o) : putNull();
        }
        template<detail::SmartPtr T> void put(const T& p) noexcept {
            put(p.get());
        }

#if LOG_HAS_EXPECTED
        template<detail::Expected T> void put(const T& e) noexcept {
            if (e.has_value()) {
                if constexpr (!std::same_as<typename detail::Clean<T>::value_type, void>) {
                    put(e.value());
                } else {
                    putSV("void");
                }
            } else {
                putSV("error:");
                put(e.error());
            }
        }
#endif

        template<detail::Variant T> void put(const T& v) noexcept {
            if (v.valueless_by_exception()) [[unlikely]] {
                putNull();
                return;
            }
            std::visit([this](const auto& x) { put(x); }, v);
        }

        void put(const std::error_code e) noexcept {
            putSV(e.category().name());
            put(':');
            put(e.value());
        }
        void put(const std::errc e) noexcept {
            put(std::make_error_code(e));
        }

        template<typename R, typename P> void put(std::chrono::duration<R, P> d) noexcept {
            put(d.count());
            constexpr std::string_view suf = [] consteval {
                if constexpr (std::same_as<P, std::nano>) {
                    return "ns";
                } else if constexpr (std::same_as<P, std::micro>) {
                    return "us";
                } else if constexpr (std::same_as<P, std::milli>) {
                    return "ms";
                } else if constexpr (std::ratio_equal_v<P, std::ratio<1>>) {
                    return "s";
                } else if constexpr (std::ratio_equal_v<P, std::ratio<60>>) {
                    return "min";
                } else if constexpr (std::ratio_equal_v<P, std::ratio<3600>>) {
                    return "h";
                } else if constexpr (std::ratio_equal_v<P, std::ratio<86400>>) {
                    return "d";
                } else {
                    std::unreachable();
                }
            }();
            putSV(suf);
        }

        template<typename C, typename D> void put(std::chrono::time_point<C, D> tp) noexcept {
            using namespace std::chrono;
            auto ms = duration_cast<milliseconds>(tp.time_since_epoch()).count();
            putSV(detail::tsCache().get<true>(static_cast<std::time_t>(ms / 1000),
                                              static_cast<unsigned>(ms % 1000)));
        }

        void put(const std::source_location l) noexcept {
            put(l.file_name());
            put(':');
            put(l.line());
        }

#if LOG_HAS_STACKTRACE
        void put(const std::stacktrace& t) noexcept {
            for (const auto& e : t) {
                putSV("\n  at ");
                put(e.description());
                if (!e.source_file().empty()) {
                    put(' ');
                    put(e.source_file());
                    put(':');
                    put(e.source_line());
                }
            }
        }
#endif

        template<detail::Aggregate T> void put(const T& v) noexcept {
            put('(');
            if constexpr (detail::Pair<T>) {
                put(v.first);
                putSV(", ");
                put(v.second);
            } else {
                [this, &v]<std::size_t... I>(std::index_sequence<I...>) {
                    ((I ? putSV(", ") : void(), put(std::get<I>(v))), ...);
                }(std::make_index_sequence<std::tuple_size_v<detail::Clean<T>>>{});
            }
            put(')');
        }

        template<detail::Rangeable R> void put(const R& r) noexcept {
            put('[');
            std::size_t n = 0;
            for (auto&& e : r | std::views::take(17)) {
                if (n == 16) {
                    putSV(", ...");
                    break;
                }
                if (n++) {
                    putSV(", ");
                }
                put(e);
            }
            put(']');
        }

        template<detail::ByteLike T> void put(std::span<T> b) noexcept {
            put('[');
            auto lim = std::min(b.size(), 32UZ);
            const auto* p = reinterpret_cast<const unsigned char*>(b.data());
            std::size_t i = 0;
#if LOG_SIMD >= 2
            while (i + 8 <= lim && rem() >= 24) {
                if (i) {
                    *pos++ = ' ';
                }
                pos = detail::simd::scatter8<true>(
                    pos,
                    detail::simd::hexEnc(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p + i))));
                i += 8;
            }
#endif
            for (; i < lim; ++i) {
                if (i) {
                    put(' ');
                }
                putHexByte(p[i]);
            }
            if (b.size() > 32) {
                putSV(" ...");
            }
            put(']');
        }

        void put(const HexDump& h) noexcept {
            if (h.data == nullptr || h.size == 0U) {
                putSV("[empty]");
                return;
            }
            const auto* p = static_cast<const unsigned char*>(h.data);
            const auto tot = std::min(h.size, h.max);
            alignas(16) std::array<char, 80> line{};
            __builtin_memset(line.data(), ' ', 80);
            for (std::size_t off = 0; off < tot; off += 16) {
                if (off != 0U) {
                    put('\n');
                }
                if (off + 64 < tot) {
                    __builtin_prefetch(p + off + 64, 0, 1);
                }
                char* hx = line.data();
                char* asc = line.data() + 55;
                auto n = std::min(16UZ, tot - off);
                const auto* row = p + off;
#if LOG_SIMD >= 2
                detail::simd::hex16(hx, static_cast<std::uint16_t>(off));
#else
                detail::copyN<2>(hx, detail::kHexPair[off >> 8 & 0xFF]);
                detail::copyN<2>(hx + 2, detail::kHexPair[off & 0xFF]);
#endif
                hx += 6;
                *asc++ = '|';
                auto scalarEnc = [&](const std::size_t start) {
                    for (std::size_t i = start; i < n; ++i) {
                        if (i == 8) {
                            hx++;
                        }
                        detail::copyN<2>(hx, detail::kHexPair[row[i]]);
                        hx += 3;
                        *asc++ = detail::toAsciiDot(row[i]);
                    }
                };
#if LOG_SIMD >= 2
                auto enc = [](const unsigned char* s, char* oh, char* oa) static {
                    auto b = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(s));
                    detail::simd::scatter8<false>(oh, detail::simd::hexEnc(b));
                    _mm_storel_epi64(reinterpret_cast<__m128i*>(oa), detail::simd::toPrint(b));
                };
                if (n >= 8) {
                    enc(row, hx, asc);
                    hx += 25;
                    asc += 8;
                    if (n >= 16) {
                        enc(row + 8, hx, asc);
                        hx += 24;
                        asc += 8;
                    } else {
                        scalarEnc(8);
                    }
                } else {
                    scalarEnc(0);
                }
#else
                scalarEnc(0);
#endif
                *asc++ = '|';
                putSV({line.data(), h.ascii ? static_cast<std::size_t>(asc - line.data()) : 55UZ});
                __builtin_memset(line.data() + 6, ' ', 49);
            }
            if (h.size > h.max) {
                putSV("\n... ("), put(h.size - h.max), putSV(" more bytes)");
            }
        }

#if LOG_HAS_MDSPAN
        template<detail::MdSpanLike T> void put(const T& m) noexcept {
            constexpr auto r = detail::Clean<T>::rank();
            put('[');
            if constexpr (r == 1) {
                for (std::size_t i = 0, l = std::min(m.extent(0), 8uz); i < l; ++i) {
                    if (i) {
                        putSV(", ");
                    }
                    put(m[i]);
                }
                if (m.extent(0) > 8) {
                    putSV(", ...");
                }
            } else if constexpr (r == 2) {
                for (std::size_t i = 0, rl = std::min(m.extent(0), 4uz); i < rl; ++i) {
                    if (i) {
                        putSV(", ");
                    }
                    put('[');
                    for (std::size_t j = 0, cl = std::min(m.extent(1), 4uz); j < cl; ++j) {
                        if (j) {
                            putSV(", ");
                        }
                        put(m[i, j]);
                    }
                    if (m.extent(1) > 4) {
                        putSV(", ...");
                    }
                    put(']');
                }
                if (m.extent(0) > 4) {
                    putSV(", ...");
                }
            } else {
                putSV("mdspan<rank=");
                put(r);
                put('>');
            }
            put(']');
        }
#endif

        void putJsonStr(const std::string_view s) noexcept {
            put('"');
            const auto* p = reinterpret_cast<const unsigned char*>(s.data());
            const auto* e = p + s.size();
            if (s.size() <= 256 && rem() >= s.size() && findEsc(p, e) == e) [[likely]] {
                detail::simd::copySmall(pos, s.data(), s.size());
                pos += s.size();
                put('"');
                return;
            }
            putEscaped(p, e);
            put('"');
        }

        [[nodiscard, gnu::always_inline]] static const unsigned char* findEsc(
            const unsigned char* p, const unsigned char* e) noexcept {
#if LOG_SIMD >= 4
            while (p + 32 <= e) {
                if (auto m = detail::simd::escMask32(p)) {
                    return p + std::countr_zero(static_cast<unsigned>(m));
                }
                p += 32;
            }
#endif
#if LOG_SIMD >= 1
            while (p + 16 <= e) {
                if (const auto m = detail::simd::escMask16(p)) {
                    return p + std::countr_zero(static_cast<unsigned>(m));
                }
                p += 16;
            }
#endif
            while (p < e && detail::kJsonEsc[*p] == 0U) {
                ++p;
            }
            return p;
        }

        void putEscaped(const unsigned char* p, const unsigned char* e) noexcept {
            while (p < e) {
                const auto* c = findEsc(p, e);
                if (auto n = static_cast<std::size_t>(c - p)) {
                    putSV({reinterpret_cast<const char*>(p), n});
                }
                if (c == e) {
                    break;
                }
                if (const auto ch = *c, esc = detail::kJsonEsc[ch]; esc != 0xFF) {
                    put('\\');
                    put(static_cast<char>(esc));
                } else {
                    putFixed(detail::kJsonUni[ch]);
                }
                p = c + 1;
            }
        }

        template<typename T> void putJsonVal(const T& v) noexcept {
            if constexpr (detail::StringLike<T>) {
                putJsonStr(toSV(v));
            } else if constexpr (std::same_as<detail::Clean<T>, bool>) {
                putSV(detail::kBool[v]);
            } else {
                put(v);
            }
        }
        template<std::size_t N> void putJsonVal(const char (&s)[N]) noexcept {
            putJsonStr({s, N - 1});
        }

#if LOG_HAS_FORMAT
        template<typename T>
            requires detail::Formattable<T> && (!std::integral<T>) && (!std::floating_point<T>) &&
                     (!std::is_pointer_v<detail::Clean<T>>) && (!detail::StringLike<T>) &&
                     (!detail::Rangeable<T>) && (!detail::Aggregate<T>) && (!detail::SmartPtr<T>) &&
                     (!detail::Variant<T>)
        void put(const T& v) noexcept {
            std::array<char, 128> t{};
            auto [p, _] = std::format_to_n(t.data(), t.size() - 1, "{}", v);
            putSV({t.data(), static_cast<std::size_t>(p - t.data())});
        }
#endif
    };

    static_assert(kBuf <= 4096);
    [[nodiscard]] static std::span<char> getBuf() noexcept {
        alignas(64) thread_local std::array<char, kBuf> b{};
        return b;
    }

#if LOG_FAST_WRITE
    static void flush(const char* d, std::size_t n) noexcept {
        [[maybe_unused]] long r{}; // NOLINT(misc-const-correctness) - asm output operand
        asm volatile("syscall"
                     : "=a"(r)
                     : "a"(SYS_write), "D"(STDERR_FILENO), "S"(d), "d"(n)
                     : "rcx", "r11", "memory");
    }
#else
    static void flush(const char* d, std::size_t n) noexcept {
        [[maybe_unused]] auto w = ::write(STDERR_FILENO, d, n);
    }
#endif

    template<typename T>
    static constexpr bool isField = requires {
        T::key;
        T::value;
    };
    template<typename... A>
    static constexpr std::size_t kFldCnt = (... + (isField<detail::Clean<A>> ? 1 : 0));
    template<typename... A> static constexpr std::size_t kMsgCnt = sizeof...(A) - kFldCnt<A...>;

    template<bool J> [[gnu::always_inline]] static void emitTs(Writer& w) noexcept {
        using namespace std::chrono;
        const auto ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        auto ts = detail::tsCache().get<J>(static_cast<std::time_t>(ms / 1000),
                                           static_cast<unsigned>(ms % 1000));
        if constexpr (J) {
            w.putSV(R"(,"ts":")");
            w.putSV(ts);
            w.put('"');
        } else {
            w.putSV(ts);
            w.put(' ');
        }
    }

    template<bool J>
    [[gnu::always_inline]] static void emitKV(Writer& w,
                                              const std::string_view k,
                                              auto&& wv,
                                              bool& first) noexcept {
        constexpr auto overhead = J ? 4UZ : 1UZ; // JSON: ,"":  Logfmt: =
        if (const auto kn = k.size(); kn <= 16 && w.rem() >= kn + overhead + (J || !first))
            [[likely]] {
            auto* p = w.pos;
            if constexpr (J) {
                *p++ = ',';
                *p++ = '"';
            } else if (!first) {
                *p++ = ' ';
            }
            __builtin_memcpy(p, k.data(), kn);
            p += kn;
            if constexpr (J) {
                *p++ = '"';
                *p++ = ':';
            } else {
                *p++ = '=';
            }
            w.pos = p;
        } else {
            if constexpr (J) {
                w.putFixed(R"(,")");
            } else if (!first) {
                w.put(' ');
            }
            w.putSV(k);
            if constexpr (J) {
                w.putFixed(R"(":)");
            } else {
                w.put('=');
            }
        }
        wv();
        first = false;
    }

    template<bool J>
    [[gnu::always_inline]] static void emitScoped(Writer& w, bool& first) noexcept {
        auto& [f, n] = detail::scopedFields();
        for (std::size_t i = 0; i < n; ++i) {
            emitKV<J>(w, f[i].key, [&] { f[i].write(&w, f[i].val, J); }, first);
        }
    }

    template<typename... A>
    [[gnu::always_inline]] static void emitMsgArgs(Writer& w, A&&... a) noexcept {
        (
            [&]<typename T>(T&& x) {
                if constexpr (!isField<detail::Clean<T>>) {
                    w.put(std::forward<T>(x));
                }
            }(std::forward<A>(a)),
            ...);
    }

    template<bool J, typename... A>
    [[gnu::always_inline]] static void emitFieldArgs(Writer& w, bool& first, A&&... a) noexcept {
        (
            [&]<typename T>(T&& x) {
                if constexpr (isField<detail::Clean<T>>) {
                    emitKV<J>(
                        w,
                        x.key,
                        [&] {
                            if constexpr (J) {
                                w.putJsonVal(x.value);
                            } else {
                                w.put(x.value);
                            }
                        },
                        first);
                }
            }(std::forward<A>(a)),
            ...);
    }

    template<bool J, typename... A>
    [[gnu::always_inline]] static void emitArgs(Writer& w, bool& first, A&&... a) noexcept {
        constexpr bool hasMsg = kMsgCnt<A...> > 0;
        if constexpr (J) {
            emitScoped<true>(w, first);
            if constexpr (hasMsg) {
                w.putSV(R"(,"msg":")");
                emitMsgArgs(w, std::forward<A>(a)...);
                w.put('"');
            }
        } else {
            emitMsgArgs(w, std::forward<A>(a)...);
            first = !hasMsg && first;
            emitScoped<false>(w, first);
        }
        emitFieldArgs<J>(w, first, std::forward<A>(a)...);
    }

    template<int L, bool J, typename... A>
    static void emitCore(Writer& w, const std::string_view ctx, A&&... a) noexcept {
        if constexpr (J) {
            w.putSV(detail::kJsonHdr[L]);
            w.putSV(ctx);
            w.put('"');
        } else {
            if (sTs) [[unlikely]] {
                emitTs<false>(w);
            }
            w.putSV(sCol ? detail::kLvlC[L] : detail::kLvl[L]);
            w.putSV(ctx);
            w.putSV("] ");
        }
        if (sTs && J) [[unlikely]] {
            emitTs<true>(w);
        }
        if (sTid) [[unlikely]] {
            w.putSV(detail::tidCache().get(J));
        }
        bool first = J || !detail::scopedFields().n;
        emitArgs<J>(w, first, std::forward<A>(a)...);
        if constexpr (J) {
            w.putSV("}\n");
        } else {
            w.put('\n');
        }
    }

    template<int L, typename... A>
    [[gnu::noinline, gnu::cold]] static void emit(std::string_view ctx, A&&... a) noexcept {
        const auto b = getBuf();
        Writer w{b};
        sFmt == JSON ? emitCore<L, true>(w, ctx, std::forward<A>(a)...)
                     : emitCore<L, false>(w, ctx, std::forward<A>(a)...);
        flush(b.data(), w.len());
    }

    template<int L, typename... A>
    [[gnu::noinline, gnu::cold]] static void emitLoc(std::string_view ctx,
                                                     const std::source_location loc,
                                                     A&&... a) noexcept {
        if (sFmt == JSON) {
            emit<L>(ctx,
                    std::forward<A>(a)...,
                    Field{"file", loc.file_name()},
                    Field{"line", loc.line()});
            return;
        }
        const auto b = getBuf();
        Writer w{b};
        emitCore<L, false>(w, ctx, std::forward<A>(a)...);
        --w.pos;
        w.putSV(" (");
        if (sCol) {
            w.putSV(detail::kDim);
        }
        w.put(loc);
        if (sCol) {
            w.putSV(detail::kReset);
        }
        w.putSV(")\n");
        flush(b.data(), w.len());
    }

#if LOG_HAS_STACKTRACE
    template<int L, typename... A>
    [[gnu::noinline, gnu::cold]] static void emitStack(std::string_view ctx,
                                                       const std::stacktrace& t,
                                                       A&&... a) noexcept {
        auto b = getBuf();
        Writer w{b};
        emitCore<L, false>(w, ctx, std::forward<A>(a)...);
        --w.pos;
        w.put(t);
        w.put('\n');
        flush(b.data(), w.len());
    }
#endif
};

} // namespace logging::inline v1

#ifndef LOG_NO_GLOBAL_USING
using logging::Log;
#endif

#define LOG_ONCE(lv, ctx, ...)                                                                     \
    do {                                                                                           \
        static bool l_;                                                                            \
        if (!l_) [[unlikely]] {                                                                    \
            l_ = true;                                                                             \
            ::logging::Log::lv(ctx, __VA_ARGS__);                                                  \
        }                                                                                          \
    } while (0)
#define LOG_FIRST_N(lv, n, ctx, ...)                                                               \
    do {                                                                                           \
        static unsigned c_;                                                                        \
        if (c_ < (n)) [[unlikely]] {                                                               \
            ++c_;                                                                                  \
            ::logging::Log::lv(ctx, __VA_ARGS__);                                                  \
        }                                                                                          \
    } while (0)
#define LOG_EVERY_N(lv, n, ctx, ...)                                                               \
    do {                                                                                           \
        static unsigned c_;                                                                        \
        if (++c_ >= (n)) [[unlikely]] {                                                            \
            c_ = 0;                                                                                \
            ::logging::Log::lv(ctx, __VA_ARGS__);                                                  \
        }                                                                                          \
    } while (0)
#define LOG_RATE_LIMITED_MS(lv, ms, ctx, ...)                                                      \
    do {                                                                                           \
        if (::logging::Log::rateLimit<__LINE__>(std::chrono::milliseconds{ms})) [[unlikely]]       \
            ::logging::Log::lv(ctx, __VA_ARGS__);                                                  \
    } while (0)
#define LOG_IF(lv, cond, ctx, ...)                                                                 \
    do {                                                                                           \
        if ((cond)) [[unlikely]]                                                                   \
            ::logging::Log::lv(ctx, __VA_ARGS__);                                                  \
    } while (0)
#define LOG_ASSERT(cond, ctx, ...)                                                                 \
    do {                                                                                           \
        if (!(cond)) [[unlikely]]                                                                  \
            ::logging::Log::error(                                                                 \
                ::logging::Log::Loc{}, ctx, "Assertion failed: " #cond " ", __VA_ARGS__);          \
    } while (0)
#define LOG_DLOG(ctx, ...)                                                                         \
    do {                                                                                           \
        if constexpr (LOG_LEVEL <= 0)                                                              \
            ::logging::Log::debug(ctx, __VA_ARGS__);                                               \
    } while (0)
#define LOG_DLOG_IF(cond, ctx, ...)                                                                \
    do {                                                                                           \
        if constexpr (LOG_LEVEL <= 0)                                                              \
            LOG_IF(debug, cond, ctx, __VA_ARGS__);                                                 \
    } while (0)
