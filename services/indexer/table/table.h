#ifndef WEB3_SEARCHER_SERVICES_INDEXER_TABLE_TABLE_H_
#define WEB3_SEARCHER_SERVICES_INDEXER_TABLE_TABLE_H_

#include <filesystem>
#include <memory>
#include <span>

#include "indexer/table/candidate.h"
#include "indexer/table/segment_merger.h"
#include "indexer/table/solidity.h"
#include "indexer/table/view_rebuilder.h"

namespace web3::indexer {

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
  std::string Search(std::string_view field, std::string_view value) const;

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

}  // namespace web3::indexer
#endif  // WEB3_SEARCHER_SERVICES_INDEXER_TABLE_TABLE_H_
