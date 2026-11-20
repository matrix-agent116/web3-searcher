#include "indexer/storage/memory_segment_manager.h"

#include <gtest/gtest.h>

#include <array>
#include <future>
#include <iresearch/index/segment_list.hpp>

#include "indexer/storage/row_codec.h"
#include "indexer/storage/snapshot.h"
#include "indexer/storage/test_utils.h"

namespace web3::indexer {
namespace {

TEST(MemorySegmentManagerTest, UndoAndResetControlWhichRowsAreFlushed) {
  duckdb::DuckDB db(nullptr);
  auto schema = test::MakeSchema();
  MemorySegmentManager manager(*db.instance, schema);
  simdjson::dom::parser parser;
  const std::array<irs::field_id, 1> match{1};
  const std::array<uint8_t, 1> modes{0};
  auto build = [&](size_t ordinal, std::string_view json) {
    return manager.Build(ordinal,
                         {test::ParseArray(parser, json), match, modes});
  };
  auto first = build(0, R"([{"id":"first"}])");
  auto second = build(1, R"([{"id":"second"}])");
  manager.Reset({first, second});
  ASSERT_EQ(manager.Docs(), 2);
  manager.UndoAppend();
  EXPECT_EQ(manager.Docs(), 1);
  manager.Append(build(1, R"([{"id":"third"}])"));
  irs::MemoryDirectory disk;
  uint64_t checkpoint = 7;
  auto list = manager.Flush(disk, checkpoint);
  auto readers = irs::OpenSegmentList(disk, list, {.db = db.instance.get()});
  ASSERT_EQ(readers.size(), 1);
  ASSERT_EQ(readers.docs_count(), 2);
  EXPECT_EQ(checkpoint, 8);
  auto id = irs::doc_limits::min();
  EXPECT_EQ(
      ReadColumn(readers[0], id, 1, duckdb::LogicalType::VARCHAR).ToString(),
      "first");
  EXPECT_EQ(ReadColumn(readers[0], id + 1, 1, duckdb::LogicalType::VARCHAR)
                .ToString(),
            "third");
  manager.Reset();
  EXPECT_EQ(manager.Docs(), 0);
  EXPECT_TRUE(manager.Flush(disk, checkpoint).empty());
  EXPECT_EQ(checkpoint, 8);
}

TEST(MemorySegmentManagerTest,
     MergeKeepsConcurrentMasksAndPatchesUntilCheckpoint) {
  duckdb::DuckDB db(nullptr);
  TableSchema schema;
  schema.fields.push_back(
      {"id", duckdb::LogicalType::VARCHAR, true, true, 1, {}});
  schema.fields.push_back(
      {"tag", duckdb::LogicalType::VARCHAR, true, true, 2, {}});
  schema.fields.push_back(
      {"value", duckdb::LogicalType::VARCHAR, true, false, 3, {}});
  schema.fields.push_back(
      {"chain", duckdb::LogicalType::BIGINT, false, true, 4, {}});
  schema.leaves = {{"id", 1}, {"tag", 2}, {"chain", 3}};
  schema.merge_segments = 3;
  Snapshot view;
  std::mutex write_mutex;
  std::promise<void> published;
  MemorySegmentManager manager(*db.instance, schema);
  auto append = [&](const char* json) {
    simdjson::dom::parser parser;
    auto rows = parser.parse(std::string(json)).get_array().value();
    const std::array<irs::field_id, 2> match{1, 4};
    auto modes = view.PlanColumnUpdates(schema, rows, match);
    const auto ordinal = manager.Docs();
    auto segment = manager.Build(ordinal, {rows, match, modes});
    view.ApplySegments(schema, ordinal, {&segment.reader, 1});
    manager.Append(std::move(segment));
  };
  append(
      R"([{"chain":1,"id":"a","tag":"old"},{"chain":1,"id":"b","tag":"b"}])");
  append(R"([{"chain":1,"id":"c","tag":"c"}])");
  append(R"([{"chain":1,"id":"d","tag":"d"}])");
  Snapshot before = view;

  // Apply writes after the worker has built the new piece, before publishing
  // it, just like foreground writes arriving while the worker is unlocked.
  manager.StartMerger(write_mutex, [&](auto inputs, const auto& merged) {
    EXPECT_EQ(inputs.size(), 3);
    EXPECT_EQ(merged.docs_count(), 4);
    append(
        R"([{"chain":1,"id":"b","tag":"b","value":"patched"},{"chain":1,"id":"c","tag":"c","value":"also patched"}])");
    append(
        R"([{"chain":1,"id":"a","tag":"new"},{"chain":1,"id":"d","tag":"new"}])");
    view.ReplaceMemorySegments(inputs, merged);
    published.set_value();
  });
  auto status = published.get_future().wait_for(std::chrono::seconds(5));
  manager.StopMerger();
  ASSERT_EQ(status, std::future_status::ready);
  EXPECT_EQ(manager.Docs(), 8);
  ASSERT_EQ(view.Segments().size(), 2);
  const auto& reader = view.Segments().front().reader;
  EXPECT_EQ(reader.live_docs_count(), 2);
  ASSERT_NE(reader.docs_mask(), nullptr);
  EXPECT_TRUE(reader.docs_mask()->Contains(irs::doc_limits::min()));
  EXPECT_TRUE(reader.docs_mask()->Contains(irs::doc_limits::min() + 3));
  auto value = [&](const irs::SubReader& r, size_t row) {
    return ReadColumn(r, irs::doc_limits::min() + row, 3,
                      duckdb::LogicalType::VARCHAR);
  };
  EXPECT_EQ(value(reader, 1).ToString(), "patched");
  EXPECT_EQ(value(reader, 2).ToString(), "also patched");
  EXPECT_EQ(before.Segments().size(), 3);
  EXPECT_TRUE(value(before.Segments()[0].reader, 1).IsNull());

  irs::MemoryDirectory disk;
  uint64_t checkpoint = 0;
  const auto manifest = manager.Flush(disk, checkpoint);
  auto reopened =
      irs::OpenSegmentList(disk, manifest, {.db = db.instance.get()});
  ASSERT_EQ(reopened.size(), 1);
  EXPECT_EQ(reopened.docs_count(), 8);
  Snapshot restored;
  restored.ApplySegments(schema, 0, reopened.GetReaders());
  ASSERT_EQ(restored.Segments().size(), 1);
  const auto& saved = restored.Segments()[0].reader;
  EXPECT_EQ(saved.live_docs_count(), 4);
  EXPECT_EQ(value(saved, 1).ToString(), "patched");
  EXPECT_EQ(value(saved, 2).ToString(), "also patched");
}

}  // namespace
}  // namespace web3::indexer
