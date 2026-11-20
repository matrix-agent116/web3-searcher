#include "searcher/table/segment_merger.h"

#include <absl/strings/str_cat.h>
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iresearch/index/file_names.hpp>
#include <iresearch/index/merge_writer.hpp>
#include <iresearch/index/segment_list.hpp>
#include <iresearch/store/memory_directory.hpp>
#include <iresearch/utils/async.hpp>
#include <iresearch/utils/directory_utils.hpp>
#include <string>
#include <thread>
#include <vector>

#include "searcher/table/test_utils.h"
#include "searcher/table/utils.h"

namespace web3::searcher {
namespace {

using SegmentMergerTest = test::SolidityFixture;

TEST_F(SegmentMergerTest,
       RemapsLateReplacementsAndColumnPatchesOntoMergedRows) {
  Upsert(
      1,
      R"([{"id":"a","tag":"old"},{"id":"b","tag":"old"},{"id":"c","tag":"old"}])");
  Upsert(2,
         R"([{"id":"a","tag":"new"},{"id":"b","tag":"old","value":"first"}])");
  const auto inputs = solidity_->ReadSnapshot();
  irs::MemoryDirectory dir;
  irs::TrackingDirectory pending(dir);
  irs::MergeWriter merger(pending, {.db = db_.instance.get()});
  std::vector<const irs::SubReader*> readers;
  for (const auto& input : inputs->Segments()) readers.push_back(&input.reader);
  merger.Reset(readers.begin(), readers.end());
  irs::SegmentMeta meta;
  meta.name = irs::FileName(1);
  ASSERT_TRUE(irs::GetBlocking(merger.Flush(meta)));
  std::string entry;
  irs::AppendSegmentList(entry, 1, meta.kinds, meta.docs_count);
  auto merged = irs::OpenSegmentList(dir, entry, {.db = db_.instance.get()});

  // New disk suffix: patch b through a preexisting hole, delete c, and
  // replace every live row contributed by the second input segment.
  Upsert(
      3,
      R"([{"id":"b","tag":"old","value":"second"},{"id":"c","tag":"changed"},{"id":"a","tag":"latest"}])");
  const auto disk = solidity_->ReadSnapshot();
  Upsert(4, R"([{"id":"b","tag":"old","value":"wal"}])");
  const auto confirmed = solidity_->ReadSnapshot();
  Upsert(5, R"([{"id":"b","tag":"last"}])");
  const auto replaced = solidity_->ReadSnapshot();
  for (const auto& view : {disk, confirmed, replaced}) {
    auto next =
        view->WithCompactedPrefix(*inputs, merger, merged.GetReaders().front());
    EXPECT_EQ(next->BlockNumber(), view->BlockNumber());
    EXPECT_EQ(next->NextOrdinal(), view->NextOrdinal());
    for (auto id : {"a", "b", "c"})
      EXPECT_EQ(next->Search(solidity_->Schema(), absl::StrCat("id:", id)),
                view->Search(solidity_->Schema(), absl::StrCat("id:", id)));
    EXPECT_EQ(next->Segments().back().reader, view->Segments().back().reader);
    if (view == replaced)
      EXPECT_GE(next->Segments().front().ordinal, inputs->NextOrdinal());
  }
  EXPECT_NE(disk->Search(solidity_->Schema(), "id:b"),
            confirmed->Search(solidity_->Schema(), "id:b"));
}

TEST_F(SegmentMergerTest, CompactsOnlyEligibleTablesAndKeepsLaterWalUpdates) {
  // One seeded segment is below the threshold of two. The worker visits this
  // table before the eligible one, so publication marks a completed visit.
  auto small_path = path_ / "small";
  std::filesystem::create_directory(small_path);
  simdjson::dom::parser parser;
  Solidity::Write(small_path, *db_.instance, test::MakeSchema(),
                  test::ParseArray(parser, R"([{"id":"seed"}])"));
  auto small =
      std::make_shared<Solidity>(small_path, db_.instance, test::MakeSchema());
  auto small_manifest = ReadManifest(small_path);
  Upsert(1, R"([{"id":"a","tag":"old"},{"id":"b"}])");
  Upsert(2, R"([{"id":"a","tag":"new"},{"id":"c"}])");
  Upsert(3, R"([{"id":"wal"}])");
  const auto before_view = solidity_->ReadSnapshot();
  const auto wal_reader = before_view->Segments().back().reader;
  auto before = ReadManifest(path_);
  irs::MMapDirectory dir(path_);
  auto old = irs::ReadSegmentList(dir, before.segments);
  ASSERT_EQ(old.size(), 2);
  {
    SegmentMerger merger({small, solidity_});
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
  const auto after_view = solidity_->ReadSnapshot();
  EXPECT_EQ(after_view->Segments().back().reader, wal_reader);
  EXPECT_EQ(before_view->Search(solidity_->Schema(), "id:wal"),
            after_view->Search(solidity_->Schema(), "id:wal"));
  for (const auto& segment : old) {
    for (const auto& file : irs::SegmentFiles(segment.name, segment.kinds)) {
      EXPECT_FALSE(std::filesystem::exists(path_ / file)) << file;
    }
  }
  Reopen();
  EXPECT_EQ(solidity_->GetBlockNumber(), 3);
  EXPECT_EQ(HitCount("a"), 1);
  EXPECT_EQ(HitCount("wal"), 1);
  auto result = solidity_->ReadSnapshot()->Search(solidity_->Schema(), "id:a");
  auto hits = parser.parse(result)["hits"].get_array().value();
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
}  // namespace web3::searcher
