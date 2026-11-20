#include "searcher/query/aggregate/snapshot_rows.h"

#include <duckdb/catalog/catalog.hpp>
#include <duckdb/function/table_function.hpp>
#include <duckdb/main/client_context_state.hpp>
#include <duckdb/parser/parsed_data/create_table_function_info.hpp>
#include <utility>

#include "searcher/query/row_scan.h"

namespace web3::searcher {
namespace {

// One connection and one immutable table view per request. Only the configured
// input columns are exposed to DuckDB, using server-generated column names.
struct InputRows : duckdb::ClientContextState {
  InputRows(std::shared_ptr<const Snapshot> view, irs::Filter::ptr predicate,
            const ScanColumns& layout)
      : snapshot(std::move(view)),
        filter(std::move(predicate)),
        columns(layout) {}
  std::shared_ptr<const Snapshot> snapshot;
  irs::Filter::ptr filter;
  const ScanColumns& columns;
};

duckdb::unique_ptr<duckdb::FunctionData> BindRows(
    duckdb::ClientContext& context, duckdb::TableFunctionBindInput&,
    duckdb::vector<duckdb::LogicalType>& types,
    duckdb::vector<duckdb::Identifier>& names) {
  auto rows = context.registered_state->Get<InputRows>(SnapshotRows::kName);
  for (const auto& type : rows->columns.types) {
    names.emplace_back(SnapshotRows::ColumnName(types.size()));
    types.push_back(type);
  }
  return duckdb::make_uniq<duckdb::TableFunctionData>();
}

struct Scan : duckdb::GlobalTableFunctionState {
  explicit Scan(const InputRows& rows)
      : cursor(*rows.snapshot, *rows.filter, rows.columns) {}
  RowScan cursor;
};

duckdb::unique_ptr<duckdb::GlobalTableFunctionState> InitRows(
    duckdb::ClientContext& context, duckdb::TableFunctionInitInput&) {
  return duckdb::make_uniq<Scan>(
      *context.registered_state->Get<InputRows>(SnapshotRows::kName));
}

void ScanRows(duckdb::ClientContext& context, duckdb::TableFunctionInput& input,
              duckdb::DataChunk& output) {
  input.global_state->Cast<Scan>().cursor.Next(output, &context);
}

}  // namespace

std::string SnapshotRows::ColumnName(std::size_t slot) {
  return "c" + std::to_string(slot);
}

void SnapshotRows::Register(duckdb::ClientContext& context) {
  duckdb::CreateTableFunctionInfo info(
      duckdb::TableFunction(kName, {}, ScanRows, BindRows, InitRows));
  duckdb::Catalog::GetSystemCatalog(context).CreateTableFunction(context, info);
}

SnapshotRows::SnapshotRows(duckdb::ClientContext& context,
                           std::shared_ptr<const Snapshot> snapshot,
                           std::unique_ptr<irs::Filter> filter,
                           const ScanColumns& columns)
    : context_(context) {
  context_.registered_state->Insert(
      kName, duckdb::make_shared_ptr<InputRows>(std::move(snapshot),
                                                std::move(filter), columns));
}

SnapshotRows::~SnapshotRows() { context_.registered_state->Remove(kName); }

}  // namespace web3::searcher
