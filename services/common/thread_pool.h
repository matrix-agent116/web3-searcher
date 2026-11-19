#ifndef WEB3_SEARCHER_SERVICES_COMMON_THREAD_POOL_H_
#define WEB3_SEARCHER_SERVICES_COMMON_THREAD_POOL_H_

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace web3 {

// A fixed number of workers share a FIFO queue. Destruction drains all
// submitted work and joins the workers.
class ThreadPool {
 public:
  explicit ThreadPool(std::size_t worker_count);

  // Multiple threads may submit work. The future holds its result or exception.
  template <typename F>
  auto Commit(F&& fn) {
    using Result = std::invoke_result_t<std::decay_t<F>&>;
    auto task =
        std::make_shared<std::packaged_task<Result()>>(std::forward<F>(fn));
    auto result = task->get_future();
    {
      std::lock_guard lock(mutex_);
      tasks_.emplace([task = std::move(task)] { (*task)(); });
    }
    ready_.notify_one();
    return result;
  }

 private:
  void Run(std::stop_token stop);

  std::mutex mutex_;
  std::condition_variable_any ready_;
  std::queue<std::function<void()>> tasks_;
  // Destroy first so workers can use the queue until they finish.
  std::vector<std::jthread> workers_;
};

}  // namespace web3

#endif  // WEB3_SEARCHER_SERVICES_COMMON_THREAD_POOL_H_
