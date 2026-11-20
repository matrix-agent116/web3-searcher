#include "searcher/query/aggregate/executor.h"

#include <duckdb/common/exception.hpp>
#include <duckdb/main/connection.hpp>
#include <duckdb/main/query_result_stream.hpp>
#include <utility>

#include "searcher/query/aggregate/count.h"
#include "searcher/query/aggregate/plan.h"
#include "searcher/query/aggregate/session.h"
#include "searcher/query/aggregate/snapshot_rows.h"
#include "searcher/table/lucene_query.h"
#include "searcher/table/row_codec.h"

namespace web3::searcher {
namespace {

class AggregateResponse {
 public:
  explicit AggregateResponse(std::span<const std::string> keys) : keys_(keys) {}

  template <typename ValueAt>
  void AppendRow(ValueAt value_at) {
    body_ += first_ ? "{" : ",{";
    first_ = false;
    for (size_t col = 0; col < keys_.size(); ++col) {
      const auto& value = value_at(col);
      // Reserve the closing row, hits array and response object.
      if (body_.size() + keys_[col].size() + value.size() + 3 >
          kMaxHttpBodyBytes)
        throw std::length_error("aggregate response too large");
      body_ += keys_[col];
      body_ += value;
    }
    body_ += '}';
  }

  std::string Finish() && {
    body_ += "]}";
    return std::move(body_);
  }

 private:
  std::span<const std::string> keys_;
  std::string body_ = R"({"ok":true,"hits":[)";
  bool first_ = true;
};

template <typename Result>
void CheckResult(const Result& result) {
  if (!result.HasError()) return;
  if (result.GetErrorType() == duckdb::ExceptionType::OUT_OF_MEMORY ||
      result.GetErrorType() == duckdb::ExceptionType::INTERRUPT)
    throw AggregateUnavailable(result.GetError());
  throw std::runtime_error(result.GetError());
}

void AppendQueryResults(duckdb::Connection& connection, const std::string& sql,
                        const AggregateSession& session,
                        AggregateResponse& response) {
  auto result = connection.Submit(sql);
  CheckResult(*result);
  duckdb::QueryResultStream<> stream(std::move(result));
  while (auto chunk = stream.Fetch()) {
    for (size_t row = 0; row < chunk->size(); ++row) {
      session.CheckDeadline();
      response.AppendRow(
          [&](size_t col) { return JsonValue(chunk->GetValue(col, row)); });
    }
  }
  CheckResult(stream);
}

}  // namespace

std::string ExecuteAggregate(const AggregatePlan& plan,
                             std::shared_ptr<const Snapshot> snapshot,
                             const TableSchema& schema,
                             std::string_view expression) {
  AggregateSession session;
  auto filter = ParseLuceneQuery(schema, expression);
  AggregateResponse response(plan.JsonKeyPrefixes());
  if (plan.IsCountOnly()) {
    const auto count =
        std::to_string(CountMatches(*snapshot, *filter, session));
    response.AppendRow([&](size_t) -> std::string_view { return count; });
  } else {
    auto& connection = session.Connection();
    SnapshotRows rows(*connection.context, std::move(snapshot),
                      std::move(filter), plan.Columns());
    AppendQueryResults(connection, plan.Sql(), session, response);
  }
  session.CheckDeadline();
  return std::move(response).Finish();
}

}  // namespace web3::searcher
