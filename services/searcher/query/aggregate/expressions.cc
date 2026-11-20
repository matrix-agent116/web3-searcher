#include "searcher/query/aggregate/expressions.h"

#include <concepts>
#include <duckdb/common/bignum.hpp>
#include <stdexcept>

#include "searcher/config/schema.h"
#include "searcher/query/aggregate/functions.h"
#include "searcher/query/aggregate/snapshot_rows.h"
#include "searcher/query/row_scan.h"

namespace web3::searcher {
namespace {

template <typename T>
std::string NumericAggregate(std::string_view op, const std::string& column) {
  if (op == "sum") return "SUM(" + column + ")";
  if (op == "min") return "MIN(" + column + ")";
  if (op == "max") return "MAX(" + column + ")";
  if (op == "avg") {
    if constexpr (std::same_as<T, duckdb::bignum_t>) {
      return std::string(kBignumMean) + "(SUM(" + column + "), COUNT(" +
             column + "))";
    }
    return "AVG(" + column + ")";
  }
  throw std::invalid_argument("unknown aggregate: " + std::string(op));
}

}  // namespace

std::string AggregateExpressionCompiler::BindColumn(std::string_view name) {
  return SnapshotRows::ColumnName(columns_.Add(schema_.StoredField(name)));
}

std::string AggregateExpressionCompiler::Compile(Json item) {
  auto op = item["op"].get_string().value();
  auto field = item["field"].get_string().value();
  if (item.get_object().value().size() != 2)
    throw std::invalid_argument("aggregate requires only op and field");
  if (op == "count" && field == "*") return "COUNT(*)";
  if (field == "*") throw std::invalid_argument("only count accepts *");
  const auto& input = schema_.StoredField(field);
  auto column = SnapshotRows::ColumnName(columns_.Add(input));
  if (op == "count") return "COUNT(" + column + ")";
  using duckdb::LogicalTypeId;
  switch (input.type.id()) {
    case LogicalTypeId::BIGNUM:
      return NumericAggregate<duckdb::bignum_t>(op, column);
    case LogicalTypeId::FLOAT:
      return NumericAggregate<float>(op, column);
    case LogicalTypeId::DOUBLE:
      return NumericAggregate<double>(op, column);
    default:
      throw std::invalid_argument(
          std::string(op) + " requires a BIGNUM, FLOAT or DOUBLE field: " +
          input.name + " has type " + input.type.ToString());
  }
}

}  // namespace web3::searcher
