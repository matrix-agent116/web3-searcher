#include "searcher/manager/manager.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <string_view>

#include "searcher/manager/test_utils.h"

namespace web3::searcher {
namespace {

using ManagerTest = test::CatalogFixture;

TEST_F(ManagerTest,
       ConcurrentInitializationSeedsOnlyOnceAndReopensWithoutSeeds) {
  constexpr std::string_view kSeeded = R"({"ok":true,"tables":2,"indexed":1})";
  constexpr std::string_view kOpened = R"({"ok":true,"tables":2,"indexed":0})";
  {
    Manager manager(path_);
    auto first =
        std::async(std::launch::async, [&] { return manager.EnsureTables(); });
    auto second = manager.EnsureTables();
    auto initial = first.get();
    EXPECT_TRUE((initial == kSeeded && second == kOpened) ||
                (initial == kOpened && second == kSeeded));
    EXPECT_EQ(manager.EnsureTables(), kOpened);
  }

  ASSERT_TRUE(std::filesystem::remove(path_ / "transactions.json"));
  ASSERT_TRUE(std::filesystem::remove(path_ / "blocks.json"));
  Manager reopened(path_);
  EXPECT_EQ(reopened.EnsureTables(), kOpened);
  EXPECT_EQ(reopened.Search("transactions", "hash:a", {}, {}),
            R"({"ok":true,"hits":[{"hash":"a","value":"seed"}]})");
}

TEST_F(ManagerTest, FailedInitializationPublishesNoTablesAndAllowsRetry) {
  ASSERT_TRUE(std::filesystem::remove(path_ / "blocks.json"));
  Manager manager(path_);
  EXPECT_THROW(manager.EnsureTables(), simdjson::simdjson_error);
  EXPECT_THROW(manager.GetBlockNumber("transactions"), std::invalid_argument);

  WriteFile("blocks.json", R"([{"hash":"block-a"}])");
  EXPECT_EQ(manager.EnsureTables(), R"({"ok":true,"tables":2,"indexed":1})");
  EXPECT_EQ(manager.Search("blocks", "*:*", {}, {}),
            R"({"ok":true,"hits":[{"hash":"block-a"}]})");
  EXPECT_EQ(manager.GetBlockNumber("transactions"), -1);
}

TEST_F(ManagerTest, CandidateAndConfirmedViewsAreIsolatedByTable) {
  Manager manager(path_);
  manager.EnsureTables();
  manager.StartMerger();
  simdjson::dom::parser keys, rows;
  const auto match = test::ParseArray(keys, R"(["hash"])");
  const std::array<std::string_view, 1> projected{"value"};
  EXPECT_EQ(manager.SubmitCandidate(
                "transactions", 7, match,
                test::ParseArray(rows, R"([{"hash":"a","value":"pending"}])")),
            1);
  EXPECT_EQ(manager.GetBlockNumber("transactions"), -1);
  EXPECT_EQ(manager.Search("transactions", "hash:a", projected, {}),
            R"({"ok":true,"hits":[{"value":"pending"}]})");
  EXPECT_EQ(manager.ConfirmBlock("blocks", 20, match,
                                 test::ParseArray(rows, R"([{"hash":"b"}])")),
            1);
  EXPECT_EQ(manager.Search("transactions", "hash:b", {}, {}),
            R"({"ok":true,"hits":[]})");
  EXPECT_EQ(manager.GetBlockNumber("transactions"), -1);

  EXPECT_EQ(manager.ConfirmBlock(
                "transactions", 7, match,
                test::ParseArray(rows, R"([{"hash":"a","value":"final"}])")),
            1);
  EXPECT_EQ(manager.GetBlockNumber("transactions"), 7);
  EXPECT_EQ(manager.GetBlockNumber("blocks"), 20);
  EXPECT_EQ(manager.Search("transactions", "hash:a", projected, {}),
            R"({"ok":true,"hits":[{"value":"final"}]})");
  EXPECT_EQ(
      manager.SubmitCandidate("transactions", 7, match,
                              test::ParseArray(rows, R"([{"hash":"late"}])")),
      0);
}

TEST_F(ManagerTest, UnknownTableIsRejectedForReadsAndWrites) {
  Manager manager(path_);
  manager.EnsureTables();
  simdjson::dom::parser keys, rows;
  auto match = test::ParseArray(keys, R"(["hash"])");
  auto docs = test::ParseArray(rows, R"([{"hash":"a"}])");
  EXPECT_THROW(manager.GetBlockNumber("missing"), std::invalid_argument);
  EXPECT_THROW(manager.Search("missing", "*:*", {}, {}), std::invalid_argument);
  EXPECT_THROW(manager.SubmitCandidate("missing", 1, match, docs),
               std::invalid_argument);
  EXPECT_THROW(manager.ConfirmBlock("missing", 1, match, docs),
               std::invalid_argument);
  EXPECT_EQ(manager.GetBlockNumber("transactions"), -1);
}

}  // namespace
}  // namespace web3::searcher
