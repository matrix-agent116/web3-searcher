#include "indexer/storage/segment_merger.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iresearch/index/file_names.hpp>
#include <iresearch/index/segment_list.hpp>
#include <thread>

#include "indexer/storage/test_utils.h"
#include "indexer/storage/utils.h"

namespace web3::indexer {
namespace {

using SegmentMergerTest = test::StorageFixture;

TEST_F(SegmentMergerTest, CompactsOnlyEligibleTablesAndKeepsLaterWalUpdates) {
  // One seeded segment is below the threshold of two. The worker visits this
  // table before the eligible one, so publication marks a completed visit.
  auto small_path = path_ / "small";
  std::filesystem::create_directory(small_path);
  simdjson::dom::parser parser;
  Storage::Write(small_path, *db_.instance, test::MakeSchema(),
                 test::ParseArray(parser, R"([{"id":"seed"}])"));
  auto small =
      std::make_shared<Storage>(small_path, db_.instance, test::MakeSchema());
  auto small_manifest = ReadManifest(small_path);
  Upsert(1, R"([{"id":"a","tag":"old"},{"id":"b"}])");
  Upsert(2, R"([{"id":"a","tag":"new"},{"id":"c"}])");
  Upsert(3, R"([{"id":"wal"}])");
  auto before = ReadManifest(path_);
  irs::MMapDirectory dir(path_);
  auto old = irs::ReadSegmentList(dir, before.segments);
  ASSERT_EQ(old.size(), 2);
  {
    SegmentMerger merger({small, storage_});
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (ReadManifest(path_).segments == before.segments) {
      ASSERT_LT(std::chrono::steady_clock::now(), deadline)
          << "waiting for disk compaction";
      EXPECT_EQ(HitCount("a"), 1);
      EXPECT_EQ(HitCount("wal"), 1);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  EXPECT_EQ(ReadManifest(small_path), small_manifest);
  auto after = ReadManifest(path_);
  EXPECT_EQ(after.block_number, 2);
  auto merged = irs::ReadSegmentList(dir, after.segments);
  ASSERT_EQ(merged.size(), 1);
  EXPECT_EQ(merged[0].docs_count, 3);
  for (const auto& segment : old) {
    for (const auto& file : irs::SegmentFiles(segment.name, segment.kinds)) {
      EXPECT_FALSE(std::filesystem::exists(path_ / file)) << file;
    }
  }
  Reopen();
  EXPECT_EQ(storage_->GetBlockNumber(), 3);
  EXPECT_EQ(HitCount("a"), 1);
  EXPECT_EQ(HitCount("wal"), 1);
  auto hits =
      parser.parse(storage_->Search("id", "a"))["hits"].get_array().value();
  EXPECT_EQ(hits.at(0)["tag"].get_string().value(), "new");
}

using SegmentCleanupTest = test::TempDirectoryFixture;

TEST_F(SegmentCleanupTest,
       ObsoleteFileCleanupPreservesBaseNewerSegmentsAndUnrelatedFiles) {
  const uint64_t base = kCompactedBase | 3;
  const auto column = [](uint64_t id) {
    return irs::SegmentFiles(irs::FileName(id), irs::kColFile).at(0);
  };
  const std::vector<std::string> removed{column(1), column(kCompactedBase | 2)};
  const std::vector<std::string> kept{column(base), column(4), "updates.3.wal",
                                      "segments", irs::FileName(1) + ".other"};
  for (const auto& file : removed) std::ofstream(path_ / file) << "old";
  for (const auto& file : kept) std::ofstream(path_ / file) << "keep";
  SegmentMerger::RemoveObsoleteSegments(path_, base);
  SegmentMerger::RemoveObsoleteSegments(path_, base);
  for (const auto& file : removed)
    EXPECT_FALSE(std::filesystem::exists(path_ / file)) << file;
  for (const auto& file : kept)
    EXPECT_TRUE(std::filesystem::exists(path_ / file)) << file;
}

}  // namespace
}  // namespace web3::indexer
