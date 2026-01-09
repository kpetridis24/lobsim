#include "simex/in_memory_sink.hpp"

void InMemoryLogSink::onFill(const FillRecord& r) {
    fills.push_back(r);
}

void InMemoryLogSink::onEventApply(const EventApplyRecord& r) {
    events.push_back(r);
}

void InMemoryLogSink::reset() {
    fills.clear();
    events.clear();
}

std::vector<FillRecord> InMemoryLogSink::drainFills() {
    std::vector<FillRecord> out;
    out.swap(fills);
    return out;
}

std::vector<EventApplyRecord> InMemoryLogSink::drainEvents() {
    std::vector<EventApplyRecord> out;
    out.swap(events);
    return out;
}

const std::vector<FillRecord>& InMemoryLogSink::getFills() const {
    return fills;
}

const std::vector<EventApplyRecord>& InMemoryLogSink::getEvents() const {
    return events;
}
