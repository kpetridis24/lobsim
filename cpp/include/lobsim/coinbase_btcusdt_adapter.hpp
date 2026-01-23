#pragma once

#include "lobsim/engine.hpp"
#include "lobsim/event_adapter.hpp"
#include "lobsim/instrument.hpp"
#include "lobsim/lob_event.hpp"

#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lobsim::replay {

struct CoinbaseBTCUSDTAdapter {
    explicit CoinbaseBTCUSDTAdapter(InstrumentSpec spec) : spec_(std::move(spec)) {}

    NormalizedLobEvent normalize(const CoinbaseBTCUSDTRawEvent& raw) const {
        NormalizedLobEvent out{};
        if (!try_normalize(raw, out)) {
            throw std::runtime_error("Failed to normalize Coinbase BTC/USDT raw event");
        }
        return out;
    }

    bool try_normalize(const CoinbaseBTCUSDTRawEvent& raw, NormalizedLobEvent& out) const {
        if (raw.ts_exchange_us < 0 || raw.ts_received_us < 0)
            return false;

        if (raw.order_id.empty())
            return false;

        if (!(raw.price >= 0.0L) || !(raw.size >= 0.0L))
            return false;

        auto px_res = spec_.price_to_ticks(raw.price);
        if (!px_res.ok)
            return false;

        auto sz_res = spec_.quantity_to_lots(raw.size);
        if (!sz_res.ok)
            return false;

        out.ts_exchange = raw.ts_exchange_us;
        out.ts_received = raw.ts_received_us;
        out.update_type = raw.update_type;
        out.side = raw.side;
        out.price_ticks = px_res.value;
        out.quantity_lots = sz_res.value;

        out.order_id = stable_order_id(raw.order_id);

        out.trader_id = UnknownTraderIdSentinel;
        out.aggressor_id = UnknownAggressorIdSentinel;
        out.update_source = UpdateSource::HISTORICAL;

        out.symbol_id = spec_.symbol;
        return true;
    }

private:
    static std::int64_t stable_order_id(std::string_view value) {
        std::uint64_t hash = 14695981039346656037ULL;
        for (unsigned char c : value) {
            hash ^= c;
            hash *= 1099511628211ULL;
        }
        return std::bit_cast<std::int64_t>(hash);
    }

    InstrumentSpec spec_{};
};

static_assert(IEventAdapter<CoinbaseBTCUSDTAdapter, CoinbaseBTCUSDTRawEvent>);

} // namespace lobsim::replay
