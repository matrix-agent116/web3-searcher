#include "common/thread_pool.h"

#include <stdexcept>

namespace web3 {

ThreadPool::ThreadPool(std::size_t worker_count) {
  if (worker_count == 0) {
    throw std::invalid_argument("a thread pool needs at least one worker");
  }
  workers_.reserve(worker_count);
  for (std::size_t i = 0; i < worker_count; ++i) {
    workers_.emplace_back([this](std::stop_token stop) { Run(stop); });
  }
}

void ThreadPool::Run(std::stop_token stop) {
  for (;;) {
    std::function<void()> task;
    {
      std::unique_lock lock(mutex_);
      if (!ready_.wait(lock, stop, [this] { return !tasks_.empty(); })) {
        return;
      }
      task = std::move(tasks_.front());
      tasks_.pop();
    }
    task();
  }
}

}  // namespace web3
