#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

#include <stdx/option.hh>

namespace ghoti::lsp {

// An unbounded multi-producer queue drained by a single consumer
template <typename T> class event_queue {
  public:
    auto push(T event) -> void {
        {
            const std::scoped_lock lock{mutex_};
            events_.emplace_back(std::move(event));
        }
        ready_.notify_one();
    }

    // Blocks until an event arrives
    [[nodiscard]] auto pop() -> T {
        std::unique_lock lock{mutex_};
        ready_.wait(lock, [this] { return !events_.empty(); });
        return take_front();
    }

    // Blocks until an event arrives or `deadline` passes, whichever comes first
    [[nodiscard]] auto pop_until(std::chrono::steady_clock::time_point deadline)
        -> stdx::option<T> {
        std::unique_lock lock{mutex_};
        if (!ready_.wait_until(lock, deadline, [this] { return !events_.empty(); })) {
            return stdx::none;
        }
        return take_front();
    }

    // Does not wait for an event, instead returning an empty payload
    [[nodiscard]] auto try_pop() -> stdx::option<T> {
        const std::scoped_lock lock{mutex_};
        if (events_.empty()) { return stdx::none; }
        return take_front();
    }

  private:
    // Must be called with `mutex_` held
    auto take_front() -> T {
        auto event{std::move(events_.front())};
        events_.pop_front();
        return event;
    }

  private:
    std::mutex              mutex_;
    std::condition_variable ready_;
    std::deque<T>           events_;
};

} // namespace ghoti::lsp
