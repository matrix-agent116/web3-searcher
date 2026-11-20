#include "indexer/storage/update_log.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "indexer/storage/test_utils.h"

namespace web3::indexer {
namespace {

using UpdateLogTest = test::TempDirectoryFixture;
using Record = std::tuple<uint64_t, std::string, std::string>;

std::uintmax_t Append(const UpdateLog& log, uint64_t block,
                      std::string_view fields, std::string_view docs) {
  simdjson::dom::parser keys, rows;
  return log.Append(block, test::ParseArray(keys, fields),
                    test::ParseArray(rows, docs), log.Size());
}

std::vector<Record> Replay(const UpdateLog& log) {
  std::vector<Record> records;
  log.Replay([&](uint64_t height, auto fields, auto docs) {
    records.emplace_back(height, simdjson::minify(fields),
                         simdjson::minify(docs));
  });
  return records;
}

TEST_F(UpdateLogTest, ReplaysOrderedBatchesAndRetiresOnlyItsGeneration) {
  UpdateLog first(path_, 4);
  EXPECT_TRUE(Replay(first).empty());
  auto end = Append(first, 0, R"(["id"])", "[]");
  EXPECT_GT(end, 0);
  EXPECT_EQ(first.Size(), end);
  Append(first, 8, R"(["chain","id"])", R"([{"id":"a","chain":1}])");
  Append(first, 8, R"(["id"])", R"([{"id":"b"}])");
  const std::vector<Record> expected{
      {0, R"(["id"])", "[]"},
      {8, R"(["chain","id"])", R"([{"id":"a","chain":1}])"},
      {8, R"(["id"])", R"([{"id":"b"}])"}};
  EXPECT_EQ(Replay(UpdateLog(path_, 4)), expected);

  UpdateLog next(path_, 5);
  Append(next, 12, R"(["id"])", "[]");
  first.Retire();
  EXPECT_EQ(first.Size(), 0);
  EXPECT_EQ(Replay(next), (std::vector<Record>{{12, R"(["id"])", "[]"}}));
}

TEST_F(UpdateLogTest, TruncatesTornTailAndCanAppendAfterRecovery) {
  UpdateLog log(path_, 0);
  auto end = Append(log, 3, R"(["id"])", "[]");
  std::ofstream(path_ / "updates.0.wal", std::ios::app)
      << R"({"block_number":4)";
  EXPECT_GT(log.Size(), end);
  EXPECT_EQ(Replay(log), (std::vector<Record>{{3, R"(["id"])", "[]"}}));
  ASSERT_EQ(log.Size(), end);
  Append(log, 5, R"(["id"])", "[]");
  EXPECT_EQ(Replay(log), (std::vector<Record>{{3, R"(["id"])", "[]"},
                                              {5, R"(["id"])", "[]"}}));
}

TEST_F(UpdateLogTest, CompleteCorruptRecordIsReportedAndPreserved) {
  UpdateLog log(path_, 0);
  Append(log, 1, R"(["id"])", "[]");
  std::ofstream(path_ / "updates.0.wal", std::ios::app) << "broken\n";
  const auto size = log.Size();
  EXPECT_THROW(Replay(log), std::runtime_error);
  EXPECT_EQ(log.Size(), size);
}

TEST_F(UpdateLogTest, OnlyUnsignedBlockHeightsAreReplayed) {
  for (const char* height : {"-1", "null", "\"4\""}) {
    SCOPED_TRACE(height);
    std::ofstream(path_ / "updates.0.wal")
        << R"({"block_number":)" << height
        << R"(,"match_field":["id"],"documents":[]})" << '\n';
    EXPECT_THROW(Replay(UpdateLog(path_, 0)), simdjson::simdjson_error);
  }
}

}  // namespace
}  // namespace web3::indexer
