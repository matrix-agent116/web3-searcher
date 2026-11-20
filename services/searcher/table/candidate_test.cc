#include "searcher/table/candidate.h"

#include <gtest/gtest.h>

#include "searcher/table/test_utils.h"

namespace web3::searcher {
namespace {

TEST(CandidateTest, ReplacingEarlierBlockReplaysLaterBlocksInHeightOrder) {
  duckdb::DuckDB db(nullptr);
  auto schema = test::MakeSchema();
  Candidate candidate(*db.instance, schema);
  auto base = std::make_shared<Snapshot>();
  std::shared_ptr<const Snapshot> published = base;
  simdjson::dom::parser keys, docs;
  auto fields = test::ParseArray(keys, R"(["id"])");
  auto put = [&](uint64_t h, std::string_view rows) {
    auto view = candidate.Replace(*base, published, h, fields,
                                  test::ParseArray(docs, rows));
    published = view ? view : base;
    return published;
  };
  put(12, R"([{"id":"one","tag":"later"}])");
  auto before = put(10, R"([{"id":"one","tag":"earlier"},{"id":"old"}])");
  ASSERT_NE(before, nullptr);
  EXPECT_EQ(put(13, "[]"), before);
  auto after = put(10, R"([{"id":"new"}])");
  ASSERT_NE(after, nullptr);
  const std::string empty = R"({"ok":true,"hits":[]})";
  EXPECT_NE(before->Search(schema, "id:old"), empty);
  EXPECT_EQ(after->Search(schema, "id:old"), empty);
  EXPECT_NE(after->Search(schema, "id:new"), empty);
  EXPECT_NE(after->Search(schema, "tag:later"), empty);
  EXPECT_EQ(before->Search(schema, "tag:earlier"), empty);
  EXPECT_EQ(after->BlockNumber(), -1);
  put(10, "[]");
  base->CommitBlock(10);
  candidate.DiscardThrough(10);
  auto remaining = candidate.Overlay(*base);
  ASSERT_NE(remaining, nullptr);
  EXPECT_EQ(remaining->Search(schema, "id:new"), empty);
  EXPECT_NE(remaining->Search(schema, "tag:later"), empty);
  candidate.DiscardThrough(12);
  EXPECT_EQ(candidate.Overlay(*base), nullptr);
}

}  // namespace
}  // namespace web3::searcher
