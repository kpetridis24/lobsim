#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace lobsim::replay {

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
    Rational tick_size;
    Rational lot_size;

    RoundingPolicy price_policy{RoundingPolicy::Strict};
    RoundingPolicy qty_policy{RoundingPolicy::Strict};

    long double strict_eps{1e-12L};

    bool is_valid() const noexcept { return tick_size.valid() && lot_size.valid(); }

    ConvertResult<std::int64_t> price_to_ticks(long double price) const {
        if (!is_valid()) {
            return ConvertResult<std::int64_t>::failure("InstrumentSpec invalid tick/lot.");
        }
        if (!(price >= 0)) {
            return ConvertResult<std::int64_t>::failure("Negative price.");
        }
        const long double x =
            price * (static_cast<long double>(tick_size.den) / static_cast<long double>(tick_size.num));
        return round_to_int64(x, price_policy, "price");
    }

    ConvertResult<std::int64_t> quantity_to_lots(long double qty) const {
        if (!is_valid()) {
            return ConvertResult<std::int64_t>::failure("InstrumentSpec invalid tick/lot.");
        }
        if (!(qty >= 0)) {
            return ConvertResult<std::int64_t>::failure("Negative qty.");
        }
        const long double x = qty * (static_cast<long double>(lot_size.den) / static_cast<long double>(lot_size.num));
        return round_to_int64(x, qty_policy, "qty");
    }

    long double ticks_to_price(std::int64_t ticks) const {
        return static_cast<long double>(ticks) *
               (static_cast<long double>(tick_size.num) / static_cast<long double>(tick_size.den));
    }

    long double lots_to_quantity(std::int64_t lots) const {
        return static_cast<long double>(lots) *
               (static_cast<long double>(lot_size.num) / static_cast<long double>(lot_size.den));
    }

private:
    ConvertResult<std::int64_t> round_to_int64(long double x, RoundingPolicy pol, const char* what) const {
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
            if (std::fabsl(x - r) > strict_eps) {
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

}; // namespace lobsim::replay
