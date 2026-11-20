#ifndef WEB3_SEARCHER_SERVICES_SEARCHER_TABLE_TABLE_H_
#define WEB3_SEARCHER_SERVICES_SEARCHER_TABLE_TABLE_H_

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "common/search_limit.h"
#include "searcher/table/candidate.h"
#include "searcher/table/segment_merger.h"
#include "searcher/table/solidity.h"
#include "searcher/table/view_rebuilder.h"

namespace web3::searcher {

class AggregatePlan;
class RowProjection;

class Table {
 public:
  ~Table();
  static std::shared_ptr<Table> Open(
      const std::filesystem::path& root,
      duckdb::shared_ptr<duckdb::DatabaseInstance> database, TableSchema schema,
      std::size_t& seeded);
  static std::unique_ptr<SegmentMerger> StartMerger(
      std::span<const std::shared_ptr<Table>> tables);

  std::size_t ConfirmBlock(uint64_t height, simdjson::dom::array fields,
                           simdjson::dom::array documents);
  std::size_t SubmitCandidate(uint64_t height, simdjson::dom::array fields,
                              simdjson::dom::array documents);
  absl::int128 GetBlockNumber() const;
  const TableSchema& Schema() const { return solidity_->Schema(); }
  std::string Aggregate(const AggregatePlan& plan,
                        std::string_view expression) const;
  std::string SearchProjected(const RowProjection& projection,
                              std::string_view expression,
                              SearchLimit limit) const;
  std::string Search(std::string_view expression,
                     std::span<const std::string_view> fields = {},
                     SearchLimit limit = {}) const;

 private:
  Table(const std::filesystem::path& path,
        duckdb::shared_ptr<duckdb::DatabaseInstance> database,
        TableSchema schema);
  Solidity::OnCommit PrepareConfirmedView(
      std::shared_ptr<const Snapshot> confirmed);

  std::shared_ptr<Solidity> solidity_;
  Candidate candidate_;
  ViewRebuilder views_;
};

}  // namespace web3::searcher
#endif  // WEB3_SEARCHER_SERVICES_SEARCHER_TABLE_TABLE_H_
