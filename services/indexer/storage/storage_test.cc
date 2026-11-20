#include "indexer/storage/storage.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>

#include "indexer/storage/test_utils.h"
#include "indexer/storage/utils.h"

namespace web3::indexer {
namespace {

using StorageTest = test::StorageFixture;

TEST_F(StorageTest, NewTableStartsAtMinusOneAfterReopen) {
  EXPECT_EQ(storage_->GetBlockNumber(), -1);
  Reopen();
  EXPECT_EQ(storage_->GetBlockNumber(), -1);
}

TEST_F(StorageTest, EmptyBlockZeroIsPublishedAndRecoveredFromWal) {
  EXPECT_EQ(Upsert(0, "[]"), 0);
  EXPECT_EQ(storage_->GetBlockNumber(), 0);
  EXPECT_EQ(ReadManifest(path_).block_number, -1);

  Reopen();
  EXPECT_EQ(storage_->GetBlockNumber(), 0);
}

TEST_F(StorageTest, TwoBatchesAtBlockZeroSurviveCheckpoint) {
  Upsert(0, R"([{"id":"first"}])");
  Upsert(0, R"([{"id":"second"}])");
  EXPECT_EQ(ReadManifest(path_).block_number, 0);

  Reopen();
  EXPECT_EQ(storage_->GetBlockNumber(), 0);
  EXPECT_EQ(HitCount("first"), 1);
  EXPECT_EQ(HitCount("second"), 1);
}

TEST_F(StorageTest, InvalidFirstBatchLeavesTableAtMinusOne) {
  EXPECT_THROW(Upsert(0, R"([{"id":"partial"},{}])"), std::invalid_argument);
  EXPECT_EQ(storage_->GetBlockNumber(), -1);
  EXPECT_EQ(HitCount("partial"), 0);

  Reopen();
  EXPECT_EQ(storage_->GetBlockNumber(), -1);
  EXPECT_EQ(HitCount("partial"), 0);
}

TEST_F(StorageTest, FailedCheckpointKeepsPreviousWalAndCanBeRetried) {
  Upsert(1, R"([{"id":"first"}])");
  const auto pending = path_ / "segments.pending";
  ASSERT_TRUE(std::filesystem::create_directory(pending));
  EXPECT_THROW(Upsert(2, R"([{"id":"second"}])"), std::exception);
  EXPECT_EQ(storage_->GetBlockNumber(), 1);
  EXPECT_EQ(HitCount("second"), 0);
  ASSERT_TRUE(std::filesystem::remove(pending));
  Reopen();
  EXPECT_EQ(HitCount("first"), 1);
  EXPECT_EQ(HitCount("second"), 0);
  EXPECT_EQ(storage_->GetBlockNumber(), 1);

  Upsert(2, R"([{"id":"second"}])");
  EXPECT_EQ(ReadManifest(path_).block_number, 2);
  Reopen();
  EXPECT_EQ(HitCount("first"), 1);
  EXPECT_EQ(HitCount("second"), 1);
}

TEST_F(StorageTest, RejectsOlderBlockWithoutPublishingItsDocuments) {
  Upsert(8, R"([{"id":"accepted"}])");
  EXPECT_THROW(Upsert(7, R"([{"id":"stale"}])"), std::invalid_argument);
  EXPECT_EQ(storage_->GetBlockNumber(), 8);
  EXPECT_EQ(HitCount("stale"), 0);

  Reopen();
  EXPECT_EQ(storage_->GetBlockNumber(), 8);
  EXPECT_EQ(HitCount("accepted"), 1);
  EXPECT_EQ(HitCount("stale"), 0);
}

TEST_F(StorageTest, CompositeMatchReplacesOnlyTheMatchingRow) {
  Upsert(
      1,
      R"([{"id":"a","tag":"x","value":"old"},{"id":"a","tag":"y","value":"keep"}])",
      R"(["id","tag"])");
  EXPECT_THROW(Upsert(2, R"([{"id":"a","value":"ambiguous"}])"),
               std::invalid_argument);
  EXPECT_EQ(storage_->GetBlockNumber(), 1);
  Upsert(2, R"([{"id":"a","tag":"x","value":"new"}])", R"(["tag","id"])");
  Reopen();
  EXPECT_EQ(HitCount("a"), 2);
  auto match_value = [&](std::string_view tag) {
    simdjson::dom::parser parser;
    auto hits =
        parser.parse(storage_->Search("tag", tag))["hits"].get_array().value();
    EXPECT_EQ(hits.size(), 1);
    return std::string(hits.at(0)["value"].get_string().value());
  };
  EXPECT_EQ(match_value("x"), "new");
  EXPECT_EQ(match_value("y"), "keep");
}

TEST_F(StorageTest, MaxBlockNumberSurvivesWalAndCheckpoint) {
  constexpr auto kMaxBlock = std::numeric_limits<uint64_t>::max();
  Upsert(12, R"([{"id":"a"},{"id":"b"}])");
  Upsert(kMaxBlock, R"([{"id":"c"}])");
  EXPECT_EQ(ReadManifest(path_).block_number, 12);
  Reopen();
  EXPECT_EQ(storage_->GetBlockNumber(), kMaxBlock);
  EXPECT_EQ(HitCount("c"), 1);

  Upsert(kMaxBlock, R"([{"id":"d"}])");
  EXPECT_EQ(ReadManifest(path_).block_number, kMaxBlock);
  Reopen();
  EXPECT_EQ(storage_->GetBlockNumber(), kMaxBlock);
  EXPECT_EQ(HitCount("c"), 1);
  EXPECT_EQ(HitCount("d"), 1);
}

}  // namespace
}  // namespace web3::indexer
