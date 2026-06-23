#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>

namespace tof_net {

template <typename T>
class LatestQueue {
  public:
    void push(T v) {
        std::lock_guard<std::mutex> lock(mu_);
        value_ = std::move(v);
        cv_.notify_all();
    }

    std::optional<T> pop_latest() {
        std::lock_guard<std::mutex> lock(mu_);
        if (!value_) return std::nullopt;
        auto out = std::move(value_);
        value_.reset();
        return out;
    }

  private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::optional<T> value_;
};

} // namespace tof_net
