#include "common/thread_pool.h"

#include <gtest/gtest.h>

#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace web3 {
namespace {

TEST(ThreadPoolTest, RunsCallableOffCallerAndReturnsItsResult) {
  ThreadPool pool(1);
  auto caller = std::this_thread::get_id();
  auto result = pool.Commit([caller, value = std::make_unique<int>(42)] {
    EXPECT_NE(std::this_thread::get_id(), caller);
    return *value;
  });
  EXPECT_EQ(result.get(), 42);
}

TEST(ThreadPoolTest, ExceptionsReachCallerAndWorkerAcceptsMoreTasks) {
  ThreadPool pool(1);
  auto failed = pool.Commit([] { throw std::runtime_error("task failed"); });
  EXPECT_THROW(failed.get(), std::runtime_error);
  EXPECT_EQ(pool.Commit([] { return 7; }).get(), 7);
}

TEST(ThreadPoolTest, SingleWorkerDrainsTasksInSubmissionOrderOnDestruction) {
  std::vector<int> completed;
  {
    ThreadPool pool(1);
    for (int i = 0; i < 128; ++i) {
      pool.Commit([&completed, i] { completed.push_back(i); });
    }
  }
  ASSERT_EQ(completed.size(), 128u);
  for (int i = 0; i < 128; ++i) EXPECT_EQ(completed[i], i);
}

}  // namespace
}  // namespace web3
