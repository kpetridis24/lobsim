#pragma once

#include "simex/engine.hpp"
#include "simex/event_adapter.hpp"
#include "simex/event_source.hpp"
#include "simex/lob_event.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

namespace simex::replay {

struct ReplayConfig {
    bool requireMonotonicTsReceived{true};
    bool failFast{true};
};

struct RunSummary {
    std::uint64_t numRawEvents{0};
    std::uint64_t numNormalizedEvents{0};
    std::uint64_t numEngineUpdates{0};
    std::uint64_t numAdapterFailures{0};

    bool hasTsRange{false};
    std::int64_t firstTsReceived{0};
    std::int64_t lastTsReceived{0};
};

class ReplaySession {
public:
    explicit ReplaySession(IMatchingEngine& engine) : engine(engine) {}

    template <typename Source, typename Adapter, typename RawEvent>
        requires IEventSource<Source, RawEvent> && IEventAdapter<Adapter, RawEvent>
    RunSummary run(Source& src, const Adapter& adapter, const ReplayConfig& cfg = {}) {
        RunSummary summary{};
        bool hasLast = false;
        std::int64_t lastTs = 0;

        RawEvent raw{};
        while (src.next(raw)) {
            ++summary.numRawEvents;
            NormalizedLobEvent ev{};

            try {
                ev = adapter.normalize(raw);
            } catch (const std::exception&) {
                ++summary.numAdapterFailures;
                if (cfg.failFast) {
                    throw;
                }
                continue;
            }

            ++summary.numNormalizedEvents;

            if (!summary.hasTsRange) {
                summary.hasTsRange = true;
                summary.firstTsReceived = ev.tsReceived;
                summary.lastTsReceived = ev.tsReceived;
            } else {
                if (ev.tsReceived < summary.firstTsReceived) {
                    summary.firstTsReceived = ev.tsReceived;
                }
                if (ev.tsReceived > summary.lastTsReceived) {
                    summary.lastTsReceived = ev.tsReceived;
                }
            }

            if (cfg.requireMonotonicTsReceived) {
                if (hasLast && ev.tsReceived < lastTs) {
                    throw std::runtime_error("ReplaySession: non-monotonic tsReceived detected.");
                }
                hasLast = true;
                lastTs = ev.tsReceived;
            }

            engine.update(ev.tsExchange, ev.tsReceived, ev.side, ev.updateType, ev.priceTicks, ev.quantityLots,
                          ev.orderId, ev.traderId, ev.aggressorId, ev.updateSource);

            ++summary.numEngineUpdates;
        }
        return summary;
    }

private:
    IMatchingEngine& engine;
};

} // namespace simex::replay
