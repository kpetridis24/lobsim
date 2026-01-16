#pragma once

#include "lobsim/log_sink.hpp"

#include <memory>
#include <string>
#include <utility>
#include <unordered_map>
#include <vector>

// Multi-book sinks can now use the base records; bookKey is populated.
class IMultiLogSink : public ILogSink {
public:
    ~IMultiLogSink() override = default;
};

// Wraps a single-book sink API and ensures bookKey is stamped.
class BookScopedSink final : public ILogSink {
public:
    BookScopedSink(std::string bookKey, IMultiLogSink* parent) : bookKey_(std::move(bookKey)), parent_(parent) {}

    void onFill(const FillRecord& r) override {
        FillRecord tagged = r;
        if (tagged.bookKey.empty()) {
            tagged.bookKey = bookKey_;
        }
        parent_->onFill(tagged);
    }
    void onEventApply(const EventApplyRecord& r) override {
        EventApplyRecord tagged = r;
        if (tagged.bookKey.empty()) {
            tagged.bookKey = bookKey_;
        }
        parent_->onEventApply(tagged);
    }
    void onDiagnostic(const DiagnosticRecord& r) override {
        DiagnosticRecord tagged = r;
        if (tagged.bookKey.empty()) {
            tagged.bookKey = bookKey_;
        }
        parent_->onDiagnostic(tagged);
    }
    void reset() override {}

private:
    std::string bookKey_;
    IMultiLogSink* parent_{nullptr};
};

class InMemoryMultiLogSink final : public IMultiLogSink {
public:
    void onFill(const FillRecord& r) override { fills_.push_back(r); }
    void onEventApply(const EventApplyRecord& r) override { events_.push_back(r); }
    void onDiagnostic(const DiagnosticRecord& r) override { diagnostics_.push_back(r); }
    void reset() override {
        fills_.clear();
        events_.clear();
        diagnostics_.clear();
    }

    const std::vector<FillRecord>& fills() const { return fills_; }
    const std::vector<EventApplyRecord>& events() const { return events_; }
    const std::vector<DiagnosticRecord>& diagnostics() const { return diagnostics_; }

    std::vector<FillRecord> drainFills() {
        std::vector<FillRecord> out;
        out.swap(fills_);
        return out;
    }

    std::vector<EventApplyRecord> drainEvents() {
        std::vector<EventApplyRecord> out;
        out.swap(events_);
        return out;
    }

    std::vector<DiagnosticRecord> drainDiagnostics() {
        std::vector<DiagnosticRecord> out;
        out.swap(diagnostics_);
        return out;
    }

private:
    std::vector<FillRecord> fills_;
    std::vector<EventApplyRecord> events_;
    std::vector<DiagnosticRecord> diagnostics_;
};
