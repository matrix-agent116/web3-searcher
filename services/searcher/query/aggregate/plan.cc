#include "searcher/query/aggregate/plan.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string_view>

#include "searcher/query/aggregate/expressions.h"
#include "searcher/query/aggregate/snapshot_rows.h"

namespace web3::searcher {
namespace {

void AppendItem(std::string& clause, const std::string& item) {
  if (!clause.empty()) clause += ", ";
  clause += item;
}

bool HasAggregate(simdjson::dom::object select) {
  return std::ranges::any_of(
      select, [](const auto& entry) { return entry.value.is_object(); });
}

struct GroupBy {
  std::set<std::string_view> fields;
  std::string sql;
};

GroupBy CompileGroupBy(Json query, AggregateExpressionCompiler& expressions) {
  GroupBy group;
  auto fields = query["group_by"];
  if (fields.error() == simdjson::NO_SUCH_FIELD) return group;
  for (auto item : fields.get_array().value()) {
    auto name = item.get_string().value();
    if (!group.fields.insert(name).second)
      throw std::invalid_argument("duplicate group_by field");
    AppendItem(group.sql, expressions.BindColumn(name));
  }
  if (group.fields.empty()) throw std::invalid_argument("empty group_by");
  return group;
}

std::string CompileSelect(simdjson::dom::object select, const GroupBy& group,
                          AggregateExpressionCompiler& expressions,
                          std::vector<std::string_view>& aliases) {
  const bool grouped = !group.fields.empty() || HasAggregate(select);
  std::string sql;
  for (auto [alias, item] : select) {
    if (alias.empty() || std::ranges::find(aliases, alias) != aliases.end())
      throw std::invalid_argument("empty or duplicate output alias");
    aliases.push_back(alias);
    if (item.is_object()) {
      AppendItem(sql, expressions.Compile(item));
    } else {
      auto name = item.get_string().value();
      if (grouped && !group.fields.contains(name))
        throw std::invalid_argument("selected field must appear in group_by");
      AppendItem(sql, expressions.BindColumn(name));
    }
  }
  if (sql.empty()) throw std::invalid_argument("empty select");
  return sql;
}

std::string CompileOrderBy(Json query,
                           std::span<const std::string_view> aliases) {
  auto terms = query["order_by"];
  if (terms.error() == simdjson::NO_SUCH_FIELD) return {};
  std::set<std::string_view> seen;
  std::string sql;
  for (auto term : terms.get_array().value()) {
    auto field = term["field"].get_string().value();
    auto direction = term["direction"].get_string().value();
    auto pos = std::ranges::find(aliases, field);
    if (term.get_object().value().size() != 2 || pos == aliases.end() ||
        !seen.insert(field).second ||
        (direction != "asc" && direction != "desc"))
      throw std::invalid_argument("invalid order_by field or direction");
    AppendItem(sql, std::to_string(pos - aliases.begin() + 1) +
                        (direction == "asc" ? " ASC" : " DESC") +
                        " NULLS LAST");
  }
  if (sql.empty()) throw std::invalid_argument("empty order_by");
  return sql;
}

}  // namespace

bool AggregatePlan::Required(Json query) {
  return query["group_by"].error() != simdjson::NO_SUCH_FIELD ||
         query["order_by"].error() != simdjson::NO_SUCH_FIELD ||
         HasAggregate(query["select"].get_object().value());
}

AggregatePlan::AggregatePlan(Json query, SearchLimit limit,
                             const TableSchema& schema) {
  AggregateExpressionCompiler expressions(schema, columns_);
  const auto group = CompileGroupBy(query, expressions);
  std::vector<std::string_view> aliases;
  sql_ = "SELECT " +
         CompileSelect(query["select"].get_object().value(), group, expressions,
                       aliases) +
         " FROM " + SnapshotRows::kName + "()";
  if (!group.sql.empty()) sql_ += " GROUP BY " + group.sql;
  auto order = CompileOrderBy(query, aliases);
  if (!order.empty()) sql_ += " ORDER BY " + order;
  sql_ += " LIMIT " + std::to_string(limit.value());

  for (auto alias : aliases)
    json_key_prefixes_.push_back((json_key_prefixes_.empty() ? "" : ",") +
                                 Quote(alias) + ":");
}

}  // namespace web3::searcher
