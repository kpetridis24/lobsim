#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace simex::replay {

enum class RoundingPolicy : std::uint8_t { Strict = 0, Nearest = 1, Floor = 2, Ceil = 3 };

struct ConvertError {
    std::string msg;
};

template <typename T> struct ConvertResult {
    T value{};

    ConvertError err{};
    bool ok{false};

    static ConvertResult success(T v) { return ConvertResult{v, ConvertError{""}, true}; }
    static ConvertResult failure(std::string m) { return ConvertResult{T{}, ConvertError{std::move(m)}, false}; }
};

struct Rational {
    std::int64_t num{0};
    std::int64_t den{1};

    constexpr bool valid() const noexcept { return den > 0 && num > 0; }
};

struct InstrumentSpec {
    std::string symbol;
    std::string venue;
    Rational tickSize;
    Rational lotSize;

    RoundingPolicy pricePolicy{RoundingPolicy::Strict};
    RoundingPolicy qtyPolicy{RoundingPolicy::Strict};

    long double strictEps{1e-12L};

    bool isValid() const noexcept { return tickSize.valid() && lotSize.valid(); }

    ConvertResult<std::int64_t> priceToTicks(long double price) const {
        if (!isValid()) {
            return ConvertResult<std::int64_t>::failure("InstrumentSpec invalid tick/lot.");
        }
        if (!(price >= 0)) {
            return ConvertResult<std::int64_t>::failure("Negative price.");
        }
        const long double x = price * (static_cast<long double>(tickSize.den) / static_cast<long double>(tickSize.num));
        return roundToInt64(x, pricePolicy, "price");
    }

    ConvertResult<std::int64_t> quantityToLots(long double qty) const {
        if (!isValid()) {
            return ConvertResult<std::int64_t>::failure("InstrumentSpec invalid tick/lot.");
        }
        if (!(qty >= 0)) {
            return ConvertResult<std::int64_t>::failure("Negative qty.");
        }
        const long double x = qty * (static_cast<long double>(lotSize.den) / static_cast<long double>(lotSize.num));
        return roundToInt64(x, qtyPolicy, "qty");
    }

    long double ticksToPrice(std::int64_t ticks) const {
        return static_cast<long double>(ticks) *
               (static_cast<long double>(tickSize.num) / static_cast<long double>(tickSize.den));
    }

    long double lotsToQuantity(std::int64_t lots) const {
        return static_cast<long double>(lots) *
               (static_cast<long double>(lotSize.num) / static_cast<long double>(lotSize.den));
    }

private:
    ConvertResult<std::int64_t> roundToInt64(long double x, RoundingPolicy pol, const char* what) const {
        if (!std::isfinite(static_cast<double>(x))) {
            return ConvertResult<std::int64_t>::failure(std::string("Non-finite ") + what + " conversion.");
        }

        long double y = 0.0L;
        switch (pol) {
        case RoundingPolicy::Nearest:
            y = std::llround(x);
            break;
        case RoundingPolicy::Floor:
            y = std::floor(x);
            break;
        case RoundingPolicy::Ceil:
            y = std::ceil(x);
            break;
        case RoundingPolicy::Strict: {
            const long double r = std::llround(x);
            if (std::fabsl(x - r) > strictEps) {
                return ConvertResult<std::int64_t>::failure(std::string("Strict ") + what + " not on grid.");
            }
            y = r;
            break;
        }
        default:
            return ConvertResult<std::int64_t>::failure("Unknown rounding policy.");
        }

        if (y < 0.0L) {
            return ConvertResult<std::int64_t>::failure(std::string("Negative ") + what + " ticks/lots.");
        }
        if (y > static_cast<long double>(std::numeric_limits<std::int64_t>::max())) {
            return ConvertResult<std::int64_t>::failure(std::string("Overflow converting ") + what + ".");
        }

        return ConvertResult<std::int64_t>::success(static_cast<std::int64_t>(y));
    }
};

}; // namespace simex::replay
