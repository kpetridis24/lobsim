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
        if (!tryNormalize(raw, out)) {
            throw std::runtime_error("Failed to normalize Coinbase BTC/USDT raw event");
        }
        return out;
    }

    bool tryNormalize(const CoinbaseBTCUSDTRawEvent& raw, NormalizedLobEvent& out) const {
        if (raw.tsExchangeUs < 0 || raw.tsReceivedUs < 0)
            return false;

        if (raw.orderId.empty())
            return false;

        if (!(raw.price >= 0.0L) || !(raw.size >= 0.0L))
            return false;

        auto pxRes = spec_.priceToTicks(raw.price);
        if (!pxRes.ok)
            return false;

        auto szRes = spec_.quantityToLots(raw.size);
        if (!szRes.ok)
            return false;

        out.tsExchange = raw.tsExchangeUs;
        out.tsReceived = raw.tsReceivedUs;
        out.updateType = raw.updateType;
        out.side = raw.side;
        out.priceTicks = pxRes.value;
        out.quantityLots = szRes.value;

        out.orderId = stableOrderId(raw.orderId);

        out.traderId = UnknownTraderIdSentinel;
        out.aggressorId = UnknownAggressorIdSentinel;
        out.updateSource = UpdateSource::HISTORICAL;

        out.symbolId = spec_.symbol;
        return true;
    }

private:
    static std::int64_t stableOrderId(std::string_view value) {
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
