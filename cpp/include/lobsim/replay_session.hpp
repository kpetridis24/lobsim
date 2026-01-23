#pragma once

#include "lobsim/engine.hpp"
#include "lobsim/event_adapter.hpp"
#include "lobsim/event_source.hpp"
#include "lobsim/lob_event.hpp"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

namespace lobsim::replay {

struct ReplayConfig {
    bool require_monotonic_ts_received{true};
    bool fail_fast{true};
};

struct RunSummary {
    std::uint64_t num_raw_events{0};
    std::uint64_t num_normalized_events{0};
    std::uint64_t num_engine_updates{0};
    std::uint64_t num_adapter_failures{0};

    bool has_ts_range{false};
    std::int64_t first_ts_received{0};
    std::int64_t last_ts_received{0};
};

class ReplaySession {
public:
    explicit ReplaySession(IMatchingEngine& engine, ReplayConfig cfg = {}) : engine(engine), cfg(cfg) {}

    void step(const NormalizedLobEvent& ev) {
        if (ev.ts_received < 0) {
            throw std::runtime_error("ReplaySession: invalid ts_received (< 0).");
        }

        if (cfg.require_monotonic_ts_received && has_last_ts_received && ev.ts_received < last_ts_received) {
            throw std::runtime_error("ReplaySession: non-monotonic ts_received detected.");
        }
        has_last_ts_received = true;
        last_ts_received = ev.ts_received;

        engine.update(ev);
    }

    RunSummary run(std::span<const NormalizedLobEvent> events, const ReplayConfig& cfg = {}) {
        this->cfg = cfg;
        RunSummary summary{};

        for (const auto& ev : events) {
            ++summary.num_normalized_events;

            if (!summary.has_ts_range) {
                summary.has_ts_range = true;
                summary.first_ts_received = ev.ts_received;
                summary.last_ts_received = ev.ts_received;
            } else {
                if (ev.ts_received < summary.first_ts_received) {
                    summary.first_ts_received = ev.ts_received;
                }
                if (ev.ts_received > summary.last_ts_received) {
                    summary.last_ts_received = ev.ts_received;
                }
            }
            step(ev);
            ++summary.num_engine_updates;
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
            ++summary.num_raw_events;
            NormalizedLobEvent ev{};

            try {
                ev = adapter.normalize(raw);
            } catch (const std::exception&) {
                ++summary.num_adapter_failures;
                if (cfg.fail_fast) {
                    throw;
                }
                continue;
            }

            ++summary.num_normalized_events;

            if (!summary.has_ts_range) {
                summary.has_ts_range = true;
                summary.first_ts_received = ev.ts_received;
                summary.last_ts_received = ev.ts_received;
            } else {
                if (ev.ts_received < summary.first_ts_received) {
                    summary.first_ts_received = ev.ts_received;
                }
                if (ev.ts_received > summary.last_ts_received) {
                    summary.last_ts_received = ev.ts_received;
                }
            }
            step(ev);

            ++summary.num_engine_updates;
        }
        return summary;
    }

private:
    IMatchingEngine& engine;
    ReplayConfig cfg{};
    bool has_last_ts_received{false};
    std::int64_t last_ts_received{0};
};

} // namespace lobsim::replay
