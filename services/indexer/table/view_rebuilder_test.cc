#include "indexer/table/view_rebuilder.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

#include "indexer/table/snapshot.h"

namespace web3::indexer {
namespace {

TEST(ViewRebuilderTest,
     ReadersKeepPreviousSnapshotUntilPreparedViewIsPublished) {
  using namespace std::chrono_literals;
  auto before = std::make_shared<Snapshot>();
  auto after = std::make_shared<Snapshot>();
  ViewRebuilder views(before);
  std::promise<void> building;
  std::promise<void> release;
  auto ready = building.get_future();
  auto submit = std::async(std::launch::async, [&] {
    const auto caller = std::this_thread::get_id();
    return views.Prepare([&] {
      EXPECT_NE(std::this_thread::get_id(), caller);
      building.set_value();
      release.get_future().wait();
      return after;
    });
  });
  if (ready.wait_for(3s) != std::future_status::ready) {
    release.set_value();
    FAIL() << "worker did not start";
  }
  auto read = std::async(std::launch::async, [&] { return views.Read(); });
  const auto status = read.wait_for(1s);
  release.set_value();
  ASSERT_EQ(status, std::future_status::ready);
  EXPECT_EQ(read.get(), before);
  auto pending = submit.get();
  EXPECT_EQ(views.Read(), before);
  views.Publish(std::move(pending));
  EXPECT_EQ(views.Read(), after);
}

TEST(ViewRebuilderTest,
     LatePublicationCannotReplaceNewerCandidateAtSameHeight) {
  auto original = std::make_shared<Snapshot>();
  auto a = std::make_shared<Snapshot>();
  auto b = std::make_shared<Snapshot>();
  a->CommitBlock(10);
  b->CommitBlock(10);
  ViewRebuilder views(original);
  auto first = views.Prepare([&] { return a; });
  auto second = views.Prepare([&] { return b; });
  EXPECT_GT(second.generation, first.generation);
  views.Publish(std::move(second));
  views.Publish(std::move(first));
  EXPECT_EQ(views.Read(), b);
}

TEST(ViewRebuilderTest, BuildExceptionLeavesPublishedViewAndWorkerUsable) {
  auto before = std::make_shared<Snapshot>();
  auto after = std::make_shared<Snapshot>();
  ViewRebuilder views(before);
  EXPECT_THROW(views.Prepare([]() -> std::shared_ptr<const Snapshot> {
    throw std::runtime_error("invalid candidate");
  }),
               std::runtime_error);
  EXPECT_EQ(views.Read(), before);
  views.Publish(views.Prepare([&] { return after; }));
  EXPECT_EQ(views.Read(), after);
}

}  // namespace
}  // namespace web3::indexer
