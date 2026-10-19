#include "driver/cmd/lsp/worker.hh"

#include <atomic>
#include <mutex>
#include <ostream>
#include <stop_token>
#include <utility>

#include <stdx/option.hh>

#include "driver/cmd/lsp/snapshot.hh"

namespace ghoti::lsp {

analysis_worker::analysis_worker(std::ostream& error_stream, finished_callback on_finished)
    : error_stream_{error_stream}, on_finished_{std::move(on_finished)},
      thread_{[this](const std::stop_token& stop) { run(stop); }} {}

analysis_worker::~analysis_worker() {
    // Stops the analysis underway at its next declaration instead of waiting for it to finish
    cancel_.store(true, std::memory_order_relaxed);
    thread_.request_stop();
}

auto analysis_worker::schedule(analysis_inputs inputs) -> void {
    {
        const std::scoped_lock lock{mutex_};
        next_ = std::move(inputs);
        cancel_.store(true, std::memory_order_relaxed);
    }
    wake_.notify_one();
}

auto analysis_worker::cancel() -> void {
    const std::scoped_lock lock{mutex_};
    next_ = stdx::none;
    cancel_.store(true, std::memory_order_relaxed);
}

auto analysis_worker::run(const std::stop_token& stop) -> void {
    while (true) {
        analysis_inputs inputs;
        {
            std::unique_lock lock{mutex_};
            if (!wake_.wait(lock, stop, [this] { return next_.has_value(); })) { return; }
            inputs = std::move(*next_);
            next_  = stdx::none;
            // Cleared under the lock, so a `schedule` or `cancel` from here on stops this analysis
            cancel_.store(false, std::memory_order_relaxed);
        }

        if (auto snapshot{analysis_snapshot::build(
                std::move(inputs), parse_cache_, error_stream_, &cancel_)}) {
            on_finished_(std::move(*snapshot));
        }
    }
}

} // namespace ghoti::lsp
