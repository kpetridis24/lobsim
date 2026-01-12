#pragma once
#include "lobsim/log_sink.hpp"

#include <vector>

class InMemoryLogSink final : public ILogSink {
public:
    void onFill(const FillRecord& r) override;
    void onEventApply(const EventApplyRecord& r) override;
    void reset() override;

    const std::vector<FillRecord>& getFills() const;
    const std::vector<EventApplyRecord>& getEvents() const;

    std::vector<FillRecord> drainFills();
    std::vector<EventApplyRecord> drainEvents();

private:
    std::vector<FillRecord> fills;
    std::vector<EventApplyRecord> events;
};
