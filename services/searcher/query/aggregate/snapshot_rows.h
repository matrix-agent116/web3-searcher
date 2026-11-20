#ifndef WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_AGGREGATE_SNAPSHOT_ROWS_H_
#define WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_AGGREGATE_SNAPSHOT_ROWS_H_

#include <cstddef>
#include <memory>
#include <string>

namespace duckdb {
class ClientContext;
}
namespace irs {
class Filter;
}

namespace web3::searcher {

struct ScanColumns;
class Snapshot;

// Bind one request's immutable view to the DuckDB table function until the
// result stream is consumed or destroyed.
class SnapshotRows {
 public:
  static constexpr char kName[] = "snapshot_rows";
  static std::string ColumnName(std::size_t slot);
  static void Register(duckdb::ClientContext& context);

  SnapshotRows(duckdb::ClientContext& context,
               std::shared_ptr<const Snapshot> snapshot,
               std::unique_ptr<irs::Filter> filter, const ScanColumns& columns);
  ~SnapshotRows();
  SnapshotRows(const SnapshotRows&) = delete;
  SnapshotRows& operator=(const SnapshotRows&) = delete;

 private:
  duckdb::ClientContext& context_;
};

}  // namespace web3::searcher

#endif  // WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_AGGREGATE_SNAPSHOT_ROWS_H_
