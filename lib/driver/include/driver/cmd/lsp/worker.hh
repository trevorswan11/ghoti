#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <ostream>
#include <stop_token>
#include <thread>

#include <stdx/memory.hh>
#include <stdx/option.hh>

#include "compiler/module/parse_cache.hh"
#include "driver/cmd/lsp/snapshot.hh"

namespace ghoti::lsp {

// Builds analyses on its own thread, one at a time, always of the newest inputs it was given.
//
// Owns the parse cache its analyses share, which no other thread touches
class analysis_worker {
  public:
    // Called on the worker thread with each analysis that ran to completion
    using finished_callback = std::function<void(stdx::box<analysis_snapshot>)>;

  public:
    analysis_worker(std::ostream& error_stream, finished_callback on_finished);
    ~analysis_worker();
    MAKE_PINNED(analysis_worker)

    // Analyzes `inputs` next, abandoning whatever older analysis is underway or waiting
    auto schedule(analysis_inputs inputs) -> void;

    // Abandons whatever analysis is underway or waiting, since newer inputs are coming
    auto cancel() -> void;

  private:
    auto run(const std::stop_token& stop) -> void;

  private:
    std::ostream&     error_stream_;
    finished_callback on_finished_;
    mod::parse_cache  parse_cache_;

    std::mutex                    mutex_;
    std::condition_variable_any   wake_;
    stdx::option<analysis_inputs> next_;
    std::atomic<bool>             cancel_{false};
    // Last, so the thread stops before anything it uses is destroyed
    std::jthread thread_;
};

} // namespace ghoti::lsp
