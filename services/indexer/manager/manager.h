#ifndef WEB3_SEARCHER_SERVICES_INDEXER_MANAGER_MANAGER_H_
#define WEB3_SEARCHER_SERVICES_INDEXER_MANAGER_MANAGER_H_

#include <absl/numeric/int128.h>

#include <duckdb/main/database.hpp>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "indexer/storage/storage.h"

namespace web3::indexer {

class Manager {
 public:
  explicit Manager(std::filesystem::path directory);

  // Publish or open the catalog before serving queries. Later calls report
  // zero imported rows and reuse the initialized tables.
  std::string EnsureTables();
  std::vector<std::shared_ptr<Storage>> GetStorages() const;
  absl::int128 GetBlockNumber(std::string_view table) const;
  std::string Upsert(std::string_view table, uint64_t block_number,
                     simdjson::dom::array match_fields,
                     simdjson::dom::array documents);
  std::string Search(std::string_view table, std::string_view field,
                     std::string_view value) const;

 private:
  Storage& GetStorage(std::string_view table) const;
  std::filesystem::path directory_;
  duckdb::DuckDB database_{nullptr};
  std::once_flag initialized_;
  std::map<std::string, std::shared_ptr<Storage>, std::less<>> tables_;
};

}  // namespace web3::indexer

#endif  // WEB3_SEARCHER_SERVICES_INDEXER_MANAGER_MANAGER_H_
