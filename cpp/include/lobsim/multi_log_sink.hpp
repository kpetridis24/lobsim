#pragma once

#include "lobsim/log_sink.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class IMultiLogSink : public ILogSink {
public:
    ~IMultiLogSink() override = default;

    virtual std::vector<FillRecord> fillsFor(std::string_view bookKey) const = 0;
    virtual std::vector<EventApplyRecord> eventsFor(std::string_view bookKey) const = 0;
    virtual std::vector<DiagnosticRecord> diagnosticsFor(std::string_view bookKey) const = 0;
};

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
    void onFill(const FillRecord& r) override {
        fillIndex_[r.bookKey].push_back(fills_.size());
        fills_.push_back(r);
    }
    void onEventApply(const EventApplyRecord& r) override {
        eventIndex_[r.bookKey].push_back(events_.size());
        events_.push_back(r);
    }
    void onDiagnostic(const DiagnosticRecord& r) override {
        diagIndex_[r.bookKey].push_back(diagnostics_.size());
        diagnostics_.push_back(r);
    }
    void reset() override {
        fills_.clear();
        events_.clear();
        diagnostics_.clear();
        fillIndex_.clear();
        eventIndex_.clear();
        diagIndex_.clear();
    }

    std::vector<FillRecord> fillsFor(std::string_view bookKey) const override {
        std::vector<FillRecord> out;
        auto it = fillIndex_.find(std::string(bookKey));
        if (it == fillIndex_.end()) {
            return out;
        }
        out.reserve(it->second.size());
        for (auto idx : it->second) {
            out.push_back(fills_[idx]);
        }
        return out;
    }

    std::vector<EventApplyRecord> eventsFor(std::string_view bookKey) const override {
        std::vector<EventApplyRecord> out;
        auto it = eventIndex_.find(std::string(bookKey));
        if (it == eventIndex_.end()) {
            return out;
        }
        out.reserve(it->second.size());
        for (auto idx : it->second) {
            out.push_back(events_[idx]);
        }
        return out;
    }

    std::vector<DiagnosticRecord> diagnosticsFor(std::string_view bookKey) const override {
        std::vector<DiagnosticRecord> out;
        auto it = diagIndex_.find(std::string(bookKey));
        if (it == diagIndex_.end()) {
            return out;
        }
        out.reserve(it->second.size());
        for (auto idx : it->second) {
            out.push_back(diagnostics_[idx]);
        }
        return out;
    }

    const std::vector<FillRecord>& fills() const { return fills_; }
    const std::vector<EventApplyRecord>& events() const { return events_; }
    const std::vector<DiagnosticRecord>& diagnostics() const { return diagnostics_; }

    std::vector<FillRecord> drainFills() {
        std::vector<FillRecord> out;
        out.swap(fills_);
        fillIndex_.clear();
        return out;
    }

    std::vector<EventApplyRecord> drainEvents() {
        std::vector<EventApplyRecord> out;
        out.swap(events_);
        eventIndex_.clear();
        return out;
    }

    std::vector<DiagnosticRecord> drainDiagnostics() {
        std::vector<DiagnosticRecord> out;
        out.swap(diagnostics_);
        diagIndex_.clear();
        return out;
    }

private:
    std::vector<FillRecord> fills_;
    std::vector<EventApplyRecord> events_;
    std::vector<DiagnosticRecord> diagnostics_;
    std::unordered_map<std::string, std::vector<std::size_t>> fillIndex_;
    std::unordered_map<std::string, std::vector<std::size_t>> eventIndex_;
    std::unordered_map<std::string, std::vector<std::size_t>> diagIndex_;
};
