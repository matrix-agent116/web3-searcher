#include "searcher/table/snapshot.h"

#include <gtest/gtest.h>

#include <array>
#include <stdexcept>
#include <vector>

#include "searcher/table/memory_segment_manager.h"
#include "searcher/table/row_codec.h"
#include "searcher/table/test_utils.h"

namespace web3::searcher {
namespace {

TEST(SnapshotTest, AdvancingHeightDoesNotChangePreviousSnapshot) {
  Snapshot snapshot;
  EXPECT_EQ(snapshot.BlockNumber(), -1);
  snapshot.AdvanceBlock(0);
  const auto previous = snapshot;
  snapshot.AdvanceBlock(7);
  snapshot.AdvanceBlock(7);
  EXPECT_THROW(snapshot.AdvanceBlock(6), std::invalid_argument);
  EXPECT_EQ(snapshot.BlockNumber(), 7);
  EXPECT_EQ(previous.BlockNumber(), 0);
}

TEST(SnapshotTest, PlansColumnPatchesAgainstExistingRowsAndEarlierBatchRows) {
  duckdb::DuckDB db(nullptr);
  auto schema = test::MakeSchema();
  MemorySegmentManager manager(*db.instance, schema);
  Snapshot view;
  const std::array<irs::field_id, 1> match{1};
  simdjson::dom::parser parser;
  auto original =
      test::ParseArray(parser, R"([{"id":"a","tag":"old","value":"before"}])");
  const std::array<uint8_t, 1> insert{0};
  auto base = manager.Build(0, {original, match, insert});
  view.ApplySegments(schema, 0, {&base.reader, 1});
  const auto before = view;

  auto docs = test::ParseArray(parser, R"([
    {"id":"a","tag":"old","value":"patch"},
    {"id":"b","tag":"new","value":"first"},
    {"id":"b","tag":"new","value":"second"},
    {"id":"a","tag":"changed","value":"replacement"}
  ])");
  auto modes = view.PlanColumnUpdates(schema, docs, match);
  ASSERT_EQ(modes, (std::vector<uint8_t>{1, 0, 1, 0}));
  auto update = manager.Build(1, {docs, match, modes});
  view.ApplySegments(schema, 1, {&update.reader, 1});
  ASSERT_EQ(view.Segments().size(), 1);
  const auto& reader = view.Segments().front().reader;
  EXPECT_EQ(reader.live_docs_count(), 2);
  auto first = irs::doc_limits::min();
  EXPECT_EQ(
      ReadColumn(reader, first + 1, 3, duckdb::LogicalType::VARCHAR).ToString(),
      "second");
  EXPECT_EQ(
      ReadColumn(reader, first + 3, 2, duckdb::LogicalType::VARCHAR).ToString(),
      "changed");
  ASSERT_EQ(before.Segments().size(), 1);
  const auto& previous = before.Segments().front().reader;
  EXPECT_EQ(previous.live_docs_count(), 1);
  EXPECT_EQ(
      ReadColumn(previous, first, 3, duckdb::LogicalType::VARCHAR).ToString(),
      "before");
}

}  // namespace
}  // namespace web3::searcher
