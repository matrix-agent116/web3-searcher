#include "indexer/table/memory_segment.h"

#include <gtest/gtest.h>

#include <array>
#include <duckdb/main/database.hpp>
#include <iresearch/store/memory_directory.hpp>
#include <stdexcept>

#include "indexer/table/row_codec.h"
#include "indexer/table/test_utils.h"
#include "indexer/table/utils.h"

namespace web3::indexer {
namespace {

TEST(MemorySegmentTest, RequiresOneUpdateModePerDocument) {
  duckdb::DuckDB db(nullptr);
  auto schema = test::MakeSchema();
  irs::MemoryDirectory directory;
  auto writer = MakeIndexWriter(directory, *db.instance);
  simdjson::dom::parser parser;
  auto docs = test::ParseArray(parser, R"([{"id":"a"},{"id":"b"}])");
  const std::array<irs::field_id, 1> match{1};
  const std::array<uint8_t, 1> modes{0};
  EXPECT_THROW(MemorySegment::Build(0, *writer, schema, {docs, match, modes}),
               std::invalid_argument);
}

TEST(MemorySegmentTest,
     SealingAnotherBatchKeepsPreviousReaderAndMatchMetadata) {
  duckdb::DuckDB db(nullptr);
  auto schema = test::MakeSchema();
  irs::MemoryDirectory directory;
  auto writer = MakeIndexWriter(directory, *db.instance);
  simdjson::dom::parser parser;
  const std::array<irs::field_id, 2> match{1, 2};
  const std::array<uint8_t, 1> insert{0}, patch{1};
  auto first = MemorySegment::Build(
      4, *writer, schema,
      {test::ParseArray(parser, R"([{"id":"first","tag":"x"}])"), match,
       insert});
  auto second = MemorySegment::Build(
      5, *writer, schema,
      {test::ParseArray(parser, R"([{"id":"second","tag":"y"}])"), match,
       patch});
  ASSERT_EQ(first.reader.docs_count(), 1);
  ASSERT_EQ(second.reader.docs_count(), 1);
  EXPECT_EQ(first.ordinal, 4);
  EXPECT_EQ(second.ordinal, 5);
  const auto id = irs::doc_limits::min();
  EXPECT_EQ(
      ReadColumn(first.reader, id, 1, duckdb::LogicalType::VARCHAR).ToString(),
      "first");
  const auto metadata =
      ReadColumn(second.reader, id, kUpdateField, UpdateMetadataType());
  const auto& fields = duckdb::StructValue::GetChildren(metadata);
  ASSERT_EQ(fields.size(), 2);
  EXPECT_TRUE(fields[0].GetValue<bool>());
  const auto& ids = duckdb::ListValue::GetChildren(fields[1]);
  ASSERT_EQ(ids.size(), 2);
  EXPECT_EQ(ids[0].GetValue<uint64_t>(), 1);
  EXPECT_EQ(ids[1].GetValue<uint64_t>(), 2);
}

}  // namespace
}  // namespace web3::indexer
