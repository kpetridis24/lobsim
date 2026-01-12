#pragma once

#include "simex/engine.hpp"
#include "simex/event_adapter.hpp"
#include "simex/instrument.hpp"
#include "simex/lob_event.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace simex::replay {

struct CoinapiCoinbaseBTCUSDTAdapter {
    explicit CoinapiCoinbaseBTCUSDTAdapter(InstrumentSpec spec) : spec_(std::move(spec)) {}

    NormalizedLobEvent normalize(const CoinapiCoinbaseBTCUSDTRawEvent& raw) const {
        NormalizedLobEvent out{};
        if (!tryNormalize(raw, out)) {
            throw std::runtime_error("Failed to normalize CoinAPI Coinbase BTC/USDT raw event");
        }
        return out;
    }

    bool tryNormalize(const CoinapiCoinbaseBTCUSDTRawEvent& raw, NormalizedLobEvent& out) const {
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

        // stable-enough id mapping for this dataset: hash the string
        out.orderId = static_cast<std::int64_t>(std::hash<std::string>{}(raw.orderId));

        out.traderId = UnknownTraderIdSentinel;
        out.aggressorId = UnknownAggressorIdSentinel;
        out.updateSource = UpdateSource::HISTORICAL;

        out.symbolId = spec_.symbol;
        return true;
    }

private:
    InstrumentSpec spec_{};
};

static_assert(IEventAdapter<CoinapiCoinbaseBTCUSDTAdapter, CoinapiCoinbaseBTCUSDTRawEvent>);

} // namespace simex::replay
