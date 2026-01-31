#pragma once

#include "blocking_queue.hpp"
#include "lobsim/log_sink.hpp"

/**
 * Sink implementation that forwards diagnostics to a queue.
 */
class DiagnosticQueueSink final : public ILogSink {
  public:
    explicit DiagnosticQueueSink(BlockingQueue<DiagnosticRecord>* out_queue)
        : out_queue_(out_queue) {}

    void on_fill([[maybe_unused]] const FillRecord& r) override {}

    void on_event_apply([[maybe_unused]] const EventApplyRecord& r) override {}

    void on_diagnostic(const DiagnosticRecord& r) override {
        if (out_queue_ == nullptr) {
            return;
        }
        out_queue_->push(r);
    }

    void reset() override {}

  private:
    BlockingQueue<DiagnosticRecord>* out_queue_ = nullptr;
};
