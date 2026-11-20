#include "indexer/storage/row_codec.h"

#include <gtest/gtest.h>

#include <iresearch/index/segment_list.hpp>
#include <iresearch/store/memory_directory.hpp>
#include <stdexcept>
#include <string>

#include "indexer/storage/test_utils.h"
#include "indexer/storage/utils.h"

namespace web3::indexer {
namespace {

TEST(RowCodecTest, StoredRowsRoundTripWithNestedTypesNullsAndEscaping) {
  using duckdb::LogicalType;
  TableSchema schema;
  schema.fields.push_back({"key", LogicalType::VARCHAR, false, true, 1, {}});
  schema.leaves.emplace("key", 1);
  auto metadata = LogicalType::STRUCT(
      {{"enabled", LogicalType::BOOLEAN},
       {"labels", LogicalType::LIST(LogicalType::VARCHAR)},
       {"pair", LogicalType::ARRAY(LogicalType::BIGINT, 2)}});
  schema.fields.push_back({"metadata", metadata, true, false, 2, {}});
  const std::string input =
      R"([{"key":"first"},{"key":"second","metadata":{"enabled":true,"labels":["a\"b",null],"pair":[-3,7]}}])";
  simdjson::dom::parser parser;
  auto docs = test::ParseArray(parser, input);
  duckdb::DuckDB db(nullptr);
  irs::MemoryDirectory dir;
  uint64_t last_id = 0;
  auto manifest =
      WriteDiskSegments(dir, *db.instance, last_id, [&](auto& batch) {
        for (auto doc : docs)
          InsertRow(batch, schema, doc.get_object().value());
      });
  auto disk = irs::OpenSegmentList(dir, manifest, {.db = db.instance.get()});
  ASSERT_EQ(disk.size(), 1);
  const auto first = irs::doc_limits::min();
  EXPECT_TRUE(ReadColumn(disk[0], first, 2, metadata).IsNull());
  simdjson::dom::parser output;
  auto row =
      output.parse(ReadRow(schema, disk[0], first + 1)).get_object().value();
  EXPECT_EQ(row["key"].error(), simdjson::NO_SUCH_FIELD);
  auto object = row["metadata"].get_object().value();
  EXPECT_TRUE(object["enabled"].get_bool().value());
  auto labels = object["labels"].get_array().value();
  ASSERT_EQ(labels.size(), 2);
  EXPECT_EQ(labels.at(0).get_string().value(), "a\"b");
  EXPECT_TRUE(labels.at(1).is_null());
  auto pair = object["pair"].get_array().value();
  ASSERT_EQ(pair.size(), 2);
  EXPECT_EQ(pair.at(0).get_int64().value(), -3);
  EXPECT_EQ(pair.at(1).get_int64().value(), 7);
}

TEST(RowCodecTest, RejectsWrongArrayLengthAndStructuredScalarInput) {
  using duckdb::LogicalType;
  simdjson::dom::parser parser;
  auto doc = parser.parse(std::string{R"({"pair":[1],"id":{"x":2}})"})
                 .get_object()
                 .value();
  Field pair{
      "pair", LogicalType::ARRAY(LogicalType::INTEGER, 2), true, false, 1, {}};
  Field id{"id", LogicalType::VARCHAR, true, true, 2, {}};
  EXPECT_THROW(InputValue(doc, pair), std::runtime_error);
  EXPECT_THROW(InputValue(doc, id), std::runtime_error);
}

}  // namespace
}  // namespace web3::indexer
