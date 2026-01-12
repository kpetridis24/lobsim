#pragma once

#include "simex/engine.hpp"
#include "simex/event_adapter.hpp"
#include "simex/event_source.hpp"
#include "simex/lob_event.hpp"

#include <cstdint>
#include <span>
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
    explicit ReplaySession(IMatchingEngine& engine, ReplayConfig cfg = {}) : engine(engine), cfg(cfg) {}

    void step(const NormalizedLobEvent& ev) {
        if (ev.tsReceived < 0) {
            throw std::runtime_error("ReplaySession: invalid tsReceived (< 0).");
        }

        if (cfg.requireMonotonicTsReceived && hasLastTsReceived && ev.tsReceived < lastTsReceived) {
            throw std::runtime_error("ReplaySession: non-monotonic tsReceived detected.");
        }
        hasLastTsReceived = true;
        lastTsReceived = ev.tsReceived;

        engine.update(ev);
    }

    RunSummary run(std::span<const NormalizedLobEvent> events, const ReplayConfig& cfg = {}) {
        this->cfg = cfg;
        RunSummary summary{};

        for (const auto& ev : events) {
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
            step(ev);
            ++summary.numEngineUpdates;
        }
        return summary;
    }

    template <typename Source, typename Adapter, typename RawEvent>
        requires IEventSource<Source, RawEvent> && IEventAdapter<Adapter, RawEvent>
    RunSummary run(Source& src, const Adapter& adapter, const ReplayConfig& cfg = {}) {
        this->cfg = cfg;
        RunSummary summary{};

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
            step(ev);

            ++summary.numEngineUpdates;
        }
        return summary;
    }

private:
    IMatchingEngine& engine;
    ReplayConfig cfg{};
    bool hasLastTsReceived{false};
    std::int64_t lastTsReceived{0};
};

} // namespace simex::replay
