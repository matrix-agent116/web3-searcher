#include "searcher/query/aggregate/plan.h"

#include <gtest/gtest.h>

#include "searcher/config/schema.h"

namespace web3::searcher {
namespace {

AggregatePlan Compile(const TableSchema& schema,
                      const std::string& definition) {
  simdjson::dom::parser parser;
  return AggregatePlan(parser.parse(definition).value(), {}, schema);
}

TEST(AggregatePlanTest, GroupAndAggregatesShareOneInputColumn) {
  TableSchema schema;
  schema.fields.push_back({.name = "fee",
                           .type = duckdb::LogicalType::DOUBLE,
                           .stored = true,
                           .id = 7});
  const auto plan = Compile(schema, R"({"group_by":["fee"],
      "select":{"f":"fee","to\"tal":{"op":"sum","field":"fee"},
                "n":{"op":"count","field":"fee"}},
      "order_by":[{"field":"to\"tal","direction":"desc"}]})");

  EXPECT_FALSE(plan.IsCountOnly());
  ASSERT_EQ(plan.Columns().projections.size(), 1u);
  EXPECT_EQ(plan.Columns().projections[0].column_id, 7);
  ASSERT_EQ(plan.JsonKeyPrefixes().size(), 3u);
  EXPECT_EQ(plan.JsonKeyPrefixes()[1], R"(,"to\"tal":)");
  EXPECT_EQ(plan.Sql(),
            "SELECT c0, SUM(c0), COUNT(c0) FROM snapshot_rows() GROUP BY c0"
            " ORDER BY 2 DESC NULLS LAST LIMIT 100");
}

TEST(AggregatePlanTest, ChecksAggregateTypesAgainstTheStoredField) {
  using duckdb::LogicalType;
  for (const LogicalType& type :
       {LogicalType(LogicalType::BIGINT), LogicalType(LogicalType::UHUGEINT),
        LogicalType::DECIMAL(18, 2), LogicalType(LogicalType::VARCHAR),
        LogicalType(LogicalType::BOOLEAN),
        LogicalType::LIST(LogicalType::DOUBLE)}) {
    SCOPED_TRACE(type.ToString());
    TableSchema schema;
    schema.fields.push_back(
        {.name = "v", .type = type, .stored = true, .id = 1});
    for (std::string op : {"sum", "avg", "min", "max"}) {
      SCOPED_TRACE(op);
      const auto query =
          R"({"select":{"n":{"field":"v","op":")" + op + R"("}}})";
      EXPECT_THROW(Compile(schema, query), std::invalid_argument);
    }
    EXPECT_NO_THROW(
        Compile(schema, R"({"select":{"n":{"field":"v","op":"count"}}})"));
  }
}

TEST(AggregatePlanTest, CountOfAllRowsNeedsNoInputColumns) {
  const auto plan = Compile({}, R"({"select":{"n":{"op":"count","field":"*"}},
      "order_by":[{"field":"n","direction":"desc"}]})");
  EXPECT_TRUE(plan.IsCountOnly());
  EXPECT_TRUE(plan.Columns().projections.empty());
}

}  // namespace
}  // namespace web3::searcher
