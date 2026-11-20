#ifndef WEB3_SEARCHER_SERVICES_INDEXER_MANAGER_MANAGER_H_
#define WEB3_SEARCHER_SERVICES_INDEXER_MANAGER_MANAGER_H_

#include <absl/numeric/int128.h>
#include <simdjson.h>

#include <cstdint>
#include <duckdb/main/database.hpp>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace web3::indexer {

class Table;
class SegmentMerger;

class Manager {
 public:
  explicit Manager(std::filesystem::path directory);

  ~Manager();

  // Publish or open the catalog before serving queries. Later calls report
  // zero imported rows and reuse the initialized tables.
  std::string EnsureTables();
  void StartMerger();
  absl::int128 GetBlockNumber(std::string_view table) const;
  std::size_t ConfirmBlock(std::string_view table, uint64_t block_number,
                           simdjson::dom::array match_fields,
                           simdjson::dom::array documents);
  std::size_t SubmitCandidate(std::string_view table, uint64_t block_number,
                              simdjson::dom::array fields,
                              simdjson::dom::array documents);
  std::string Search(std::string_view table, std::string_view field,
                     std::string_view value) const;

 private:
  Table& GetTable(std::string_view table) const;
  std::filesystem::path directory_;
  duckdb::DuckDB database_{nullptr};
  std::once_flag initialized_;
  std::map<std::string, std::shared_ptr<Table>, std::less<>> tables_;
  std::unique_ptr<SegmentMerger> merger_;
};

}  // namespace web3::indexer

#endif  // WEB3_SEARCHER_SERVICES_INDEXER_MANAGER_MANAGER_H_
