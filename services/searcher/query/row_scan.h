#ifndef WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_ROW_SCAN_H_
#define WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_ROW_SCAN_H_

#include <cstdint>
#include <iresearch/index/hit_batcher.hpp>
#include <iresearch/search/filters/filter.hpp>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "common/search_limit.h"

namespace web3::searcher {

struct Field;
class Snapshot;
struct TableSchema;

struct ScanColumns {
  size_t Add(const Field& field);
  std::vector<irs::ColumnstoreProjection> projections;
  std::vector<duckdb::LogicalType> types;
};

// A cursor over the readers of one pinned snapshot. The caller owns the
// snapshot, filter and column layout until the last batch has been consumed.
class RowScan {
 public:
  RowScan(const Snapshot& snapshot, const irs::Filter& filter,
          const ScanColumns& columns,
          uint64_t limit = std::numeric_limits<uint64_t>::max());
  bool Next(duckdb::DataChunk& batch, duckdb::ClientContext* context = nullptr);

 private:
  bool OpenSegment(duckdb::ClientContext* context);
  const Snapshot& snapshot_;
  const irs::Filter& filter_;
  irs::HitBatcher batcher_;
  size_t segment_ = 0;
  uint64_t remaining_;
  irs::QueryBuilder::ptr query_;
  irs::docs::Root::ptr docs_;
  irs::lead::Node::ptr sparse_;
  irs::doc_id_t next_ = 0;
  irs::doc_id_t end_ = 0;
};

class RowProjection {
 public:
  void Add(const Field& field, std::string_view alias);
  std::string Search(const Snapshot& snapshot, const TableSchema& schema,
                     std::string_view expression, SearchLimit limit) const;

 private:
  ScanColumns columns_;
  struct Output {
    size_t slot;
    std::string key;
  };
  std::vector<Output> outputs_;
};

}  // namespace web3::searcher
#endif  // WEB3_SEARCHER_SERVICES_SEARCHER_QUERY_ROW_SCAN_H_
